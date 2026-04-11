#pragma once
/**
 * @file TSDB.hpp
 * @brief Custom time-series database for log entries.
 *
 * Storage model:
 *   - In-memory: unordered_map<app_name, vector<TsdbEntry>> sorted by timestamp_ns
 *   - Thread-safe: shared_mutex (multiple readers, single writer)
 *   - Auto-eviction: background thread deletes entries older than 24 hours
 *   - Persistence: binary file written every 60 seconds (atomic rename)
 *   - On startup: loads from binary file, discards entries older than 24h
 *
 * Binary file format:
 *   [8 bytes magic] [4 bytes num_apps]
 *   per app: [4 bytes app_len][app_len bytes app_name][4 bytes num_entries]
 *   per entry: [8 bytes timestamp_ns][1 byte level_idx][4 bytes msg_len][msg_len bytes msg]
 */

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>

namespace logmonitor {

// ─── TsdbEntry ───

struct TsdbEntry {
    std::string app_name;
    std::string level;        // "ERROR", "WARN", "INFO", "CRITICAL"
    std::string message;
    int64_t     timestamp_ns; // nanoseconds since Unix epoch
};

// ─── TSDB ───

class TSDB {
public:
    static constexpr int64_t EVICTION_AGE_NS = 24LL * 3600 * 1'000'000'000LL;
    static constexpr int      PERSIST_INTERVAL_S = 60;
    static constexpr int      EVICT_INTERVAL_S   = 60;

    explicit TSDB(std::string persistence_path, std::atomic<bool>& stop_flag)
        : persistence_path_(std::move(persistence_path))
        , stop_flag_(stop_flag)
    {
        // Ensure parent directory exists
        auto parent = std::filesystem::path(persistence_path_).parent_path();
        if (!parent.empty() && !std::filesystem::exists(parent)) {
            std::error_code ec;
            std::filesystem::create_directories(parent, ec);
        }
        load_from_disk();
    }

    ~TSDB() { stop(); }

    void start() {
        eviction_thread_   = std::thread([this]{ eviction_loop();   });
        persistence_thread_ = std::thread([this]{ persistence_loop();});
    }

    void stop() {
        if (eviction_thread_.joinable())    eviction_thread_.join();
        if (persistence_thread_.joinable()) persistence_thread_.join();
        // Final persist on shutdown
        write_to_disk();
    }

    // ─── Write ───

    void insert(TsdbEntry entry) {
        std::unique_lock<std::shared_mutex> lock(rw_mutex_);
        // Capture before move
        const std::string app_name = entry.app_name;
        const int64_t ts_ns = entry.timestamp_ns;
        auto& vec = data_[app_name];
        // Keep sorted by timestamp (append; entries usually arrive in order)
        if (vec.empty() || vec.back().timestamp_ns <= ts_ns) {
            vec.push_back(std::move(entry));
        } else {
            // Out-of-order: insert at correct position
            auto it = std::lower_bound(vec.begin(), vec.end(), ts_ns,
                [](const TsdbEntry& e, int64_t ts){ return e.timestamp_ns < ts; });
            vec.insert(it, std::move(entry));
        }
        // Update last_seen
        last_seen_[app_name] = std::max(last_seen_[app_name], ts_ns);
    }

    // ─── Read ───

    std::vector<TsdbEntry> getLogs(const std::string& app,
                                    const std::string& level,
                                    int64_t last_n_seconds) const {
        std::shared_lock<std::shared_mutex> lock(rw_mutex_);
        auto it = data_.find(app);
        if (it == data_.end()) return {};

        int64_t cutoff = now_ns() - last_n_seconds * 1'000'000'000LL;
        std::vector<TsdbEntry> result;
        for (const auto& e : it->second) {
            if (e.timestamp_ns < cutoff) continue;
            if (!level.empty() && e.level != level) continue;
            result.push_back(e);
        }
        return result;
    }

    int getCount(const std::string& app,
                 const std::string& level,
                 int64_t last_n_seconds) const {
        std::shared_lock<std::shared_mutex> lock(rw_mutex_);
        auto it = data_.find(app);
        if (it == data_.end()) return 0;

        int64_t cutoff = now_ns() - last_n_seconds * 1'000'000'000LL;
        int count = 0;
        for (const auto& e : it->second) {
            if (e.timestamp_ns < cutoff) continue;
            if (!level.empty() && e.level != level) continue;
            ++count;
        }
        return count;
    }

    /// Returns per-level counts for a given app over last_n_seconds
    std::map<std::string, int> getCountsByLevel(const std::string& app,
                                                  int64_t last_n_seconds) const {
        std::shared_lock<std::shared_mutex> lock(rw_mutex_);
        std::map<std::string, int> counts;
        counts["INFO"]     = 0;
        counts["WARN"]     = 0;
        counts["ERROR"]    = 0;
        counts["CRITICAL"] = 0;

        auto it = data_.find(app);
        if (it == data_.end()) return counts;

        int64_t cutoff = now_ns() - last_n_seconds * 1'000'000'000LL;
        for (const auto& e : it->second) {
            if (e.timestamp_ns < cutoff) continue;
            counts[e.level]++;
        }
        return counts;
    }

    std::vector<std::string> getApps() const {
        std::shared_lock<std::shared_mutex> lock(rw_mutex_);
        std::vector<std::string> apps;
        apps.reserve(data_.size());
        for (const auto& [app, _] : data_) {
            apps.push_back(app);
        }
        return apps;
    }

    int64_t getLastSeen(const std::string& app) const {
        std::shared_lock<std::shared_mutex> lock(rw_mutex_);
        auto it = last_seen_.find(app);
        return it != last_seen_.end() ? it->second : 0;
    }

    std::size_t total_entries() const {
        std::shared_lock<std::shared_mutex> lock(rw_mutex_);
        std::size_t total = 0;
        for (const auto& [_, vec] : data_) total += vec.size();
        return total;
    }

    // ─── JSON helpers for HTTP endpoints ───

    std::string query_to_json(const std::string& app,
                               const std::string& level,
                               int last_n_seconds) const {
        auto entries = getLogs(app, level, static_cast<int64_t>(last_n_seconds));
        nlohmann::json arr = nlohmann::json::array();
        for (const auto& e : entries) {
            nlohmann::json obj;
            obj["app"]          = e.app_name;
            obj["level"]        = e.level;
            obj["message"]      = e.message;
            obj["timestamp_ns"] = e.timestamp_ns;
            arr.push_back(std::move(obj));
        }
        return arr.dump();
    }

    std::string stats_to_json(const std::string& app, int last_n_seconds) const {
        auto counts = getCountsByLevel(app, static_cast<int64_t>(last_n_seconds));
        nlohmann::json j;
        j["app"]          = app;
        j["last_seconds"] = last_n_seconds;
        j["counts"]       = counts;
        return j.dump();
    }

    std::string apps_to_json() const {
        auto apps = getApps();
        nlohmann::json arr = nlohmann::json::array();
        for (const auto& app : apps) {
            nlohmann::json obj;
            obj["app"]         = app;
            obj["last_seen_ns"]= getLastSeen(app);
            obj["total"]       = getCount(app, "", 3600);
            arr.push_back(std::move(obj));
        }
        return arr.dump();
    }

    std::string metrics_to_prometheus() const {
        auto apps   = getApps();
        std::string out;
        out += "# HELP log_count Total log entries ingested\n";
        out += "# TYPE log_count counter\n";
        for (const auto& app : apps) {
            for (const char* lvl : {"INFO", "WARN", "ERROR", "CRITICAL"}) {
                int cnt = getCount(app, lvl, 3600);
                out += "log_count{app=\"" + app + "\",level=\"" + lvl + "\"} "
                     + std::to_string(cnt) + "\n";
            }
        }
        return out;
    }

private:
    // ─── Background threads ───

    void eviction_loop() {
        int ticks = 0;
        while (!stop_flag_.load(std::memory_order_relaxed)) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            if (++ticks < EVICT_INTERVAL_S) continue;
            ticks = 0;
            evict_old_entries();
        }
    }

    void persistence_loop() {
        int ticks = 0;
        while (!stop_flag_.load(std::memory_order_relaxed)) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            if (++ticks < PERSIST_INTERVAL_S) continue;
            ticks = 0;
            write_to_disk();
        }
    }

    void evict_old_entries() {
        int64_t cutoff = now_ns() - EVICTION_AGE_NS;
        std::unique_lock<std::shared_mutex> lock(rw_mutex_);
        for (auto& [app, vec] : data_) {
            vec.erase(
                std::remove_if(vec.begin(), vec.end(),
                    [cutoff](const TsdbEntry& e){ return e.timestamp_ns < cutoff; }),
                vec.end());
        }
        // Remove apps with no entries left
        for (auto it = data_.begin(); it != data_.end();) {
            if (it->second.empty()) it = data_.erase(it);
            else ++it;
        }
    }

    // ─── Binary persistence ───

    static constexpr uint64_t MAGIC = 0x4C4D54534442'0001ULL; // "LMTSDB\x00\x01"

    void write_to_disk() const {
        std::string tmp_path = persistence_path_ + ".tmp";
        try {
            std::ofstream out(tmp_path, std::ios::binary | std::ios::trunc);
            if (!out.is_open()) return;

            std::shared_lock<std::shared_mutex> lock(rw_mutex_);
            write_binary(out);
            out.close();
            lock.unlock();

            std::filesystem::rename(tmp_path, persistence_path_);
        } catch (const std::exception& ex) {
            std::cerr << "[TSDB] Failed to persist: " << ex.what() << "\n";
        }
    }

    void load_from_disk() {
        if (!std::filesystem::exists(persistence_path_)) return;
        try {
            std::ifstream in(persistence_path_, std::ios::binary);
            if (!in.is_open()) return;
            read_binary(in);
            // Evict stale entries loaded from file
            evict_old_entries();
            std::cout << "[TSDB] Loaded " << total_entries()
                      << " entries from disk\n";
        } catch (const std::exception& ex) {
            std::cerr << "[TSDB] Failed to load (starting empty): "
                      << ex.what() << "\n";
            std::unique_lock<std::shared_mutex> lock(rw_mutex_);
            data_.clear();
            last_seen_.clear();
        }
    }

    void write_binary(std::ofstream& out) const {
        // Magic
        out.write(reinterpret_cast<const char*>(&MAGIC), 8);

        // Number of apps
        auto num_apps = static_cast<uint32_t>(data_.size());
        out.write(reinterpret_cast<const char*>(&num_apps), 4);

        for (const auto& [app, vec] : data_) {
            // App name
            auto app_len = static_cast<uint32_t>(app.size());
            out.write(reinterpret_cast<const char*>(&app_len), 4);
            out.write(app.data(), static_cast<std::streamsize>(app_len));

            // Number of entries
            auto num_entries = static_cast<uint32_t>(vec.size());
            out.write(reinterpret_cast<const char*>(&num_entries), 4);

            for (const auto& e : vec) {
                // timestamp_ns
                out.write(reinterpret_cast<const char*>(&e.timestamp_ns), 8);

                // level (1-byte index: 0=INFO,1=WARN,2=ERROR,3=CRITICAL)
                uint8_t lvl_idx = level_to_idx(e.level);
                out.write(reinterpret_cast<const char*>(&lvl_idx), 1);

                // message
                auto msg_len = static_cast<uint32_t>(e.message.size());
                out.write(reinterpret_cast<const char*>(&msg_len), 4);
                out.write(e.message.data(), static_cast<std::streamsize>(msg_len));
            }
        }
    }

    void read_binary(std::ifstream& in) {
        uint64_t magic = 0;
        in.read(reinterpret_cast<char*>(&magic), 8);
        if (magic != MAGIC) {
            throw std::runtime_error("TSDB binary: wrong magic number");
        }

        uint32_t num_apps = 0;
        in.read(reinterpret_cast<char*>(&num_apps), 4);

        std::unique_lock<std::shared_mutex> lock(rw_mutex_);
        for (uint32_t a = 0; a < num_apps; ++a) {
            uint32_t app_len = 0;
            in.read(reinterpret_cast<char*>(&app_len), 4);
            std::string app(app_len, '\0');
            in.read(app.data(), static_cast<std::streamsize>(app_len));

            uint32_t num_entries = 0;
            in.read(reinterpret_cast<char*>(&num_entries), 4);

            auto& vec = data_[app];
            vec.reserve(num_entries);

            for (uint32_t i = 0; i < num_entries; ++i) {
                TsdbEntry e;
                e.app_name = app;

                in.read(reinterpret_cast<char*>(&e.timestamp_ns), 8);

                uint8_t lvl_idx = 0;
                in.read(reinterpret_cast<char*>(&lvl_idx), 1);
                e.level = idx_to_level(lvl_idx);

                uint32_t msg_len = 0;
                in.read(reinterpret_cast<char*>(&msg_len), 4);
                e.message.resize(msg_len);
                in.read(e.message.data(), static_cast<std::streamsize>(msg_len));

                last_seen_[app] = std::max(last_seen_[app], e.timestamp_ns);
                vec.push_back(std::move(e));
            }
        }
    }

    static uint8_t level_to_idx(const std::string& level) {
        if (level == "WARN")     return 1;
        if (level == "ERROR")    return 2;
        if (level == "CRITICAL") return 3;
        return 0; // INFO
    }

    static std::string idx_to_level(uint8_t idx) {
        switch (idx) {
            case 1: return "WARN";
            case 2: return "ERROR";
            case 3: return "CRITICAL";
            default: return "INFO";
        }
    }

    static int64_t now_ns() {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
    }

    // ─── Members ───
    std::string persistence_path_;
    std::atomic<bool>& stop_flag_;

    mutable std::shared_mutex rw_mutex_;
    std::unordered_map<std::string, std::vector<TsdbEntry>> data_;
    std::unordered_map<std::string, int64_t> last_seen_;

    std::thread eviction_thread_;
    std::thread persistence_thread_;
};

} // namespace logmonitor
