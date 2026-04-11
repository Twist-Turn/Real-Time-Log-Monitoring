#pragma once
/**
 * @file AlertRulesEngine.hpp
 * @brief Threshold-based alert rules engine with hot-reload support.
 *
 * Loads rules from alerts.json:
 *   [{"id":"rule_001","app":"myapp","level":"ERROR","threshold":5,
 *     "window_seconds":60,"notify_url":"...","cooldown_seconds":300}]
 *
 * Background thread checks rules every 10 seconds:
 *   - Queries TSDB for count(app, level, window)
 *   - If count > threshold and state is INACTIVE or COOLDOWN-expired → FIRING
 *   - After firing, enters COOLDOWN for cooldown_seconds
 *
 * Hot reload: monitors alerts.json via kqueue (macOS) / inotify (Linux).
 * On file change, reloads rules without restart. In-flight cooldowns are
 * preserved for rules whose IDs remain unchanged.
 *
 * Note: FiredAlert is defined here so AlertDelivery (Phase 4) can include
 * this header without a circular dependency.
 */

#include "TSDB.hpp"
#include "TenantManager.hpp"

#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

// Platform-specific for hot-reload file watching
#ifdef __linux__
    #include <sys/inotify.h>
    #include <unistd.h>
    #include <fcntl.h>
    #include <sys/select.h>
#elif defined(__APPLE__)
    #include <sys/event.h>
    #include <sys/time.h>
    #include <sys/types.h>
    #include <fcntl.h>
    #include <unistd.h>
#endif

namespace logmonitor {

// Forward declaration (Phase 4)
class AlertDelivery;

// ─── Alert state ───
enum class AlertState : uint8_t { INACTIVE, FIRING, COOLDOWN };

inline const char* alert_state_to_string(AlertState s) {
    switch (s) {
        case AlertState::INACTIVE: return "INACTIVE";
        case AlertState::FIRING:   return "FIRING";
        case AlertState::COOLDOWN: return "COOLDOWN";
    }
    return "UNKNOWN";
}

// ─── Alert rule configuration (loaded from alerts.json) ───
// Named AlertRuleConfig to avoid collision with PatternEngine::AlertRule
struct AlertRuleConfig {
    std::string id;
    std::string app;
    std::string level;
    int         threshold{5};
    int         window_seconds{60};
    std::string notify_url;
    int         cooldown_seconds{300};
    std::string project_id;  // optional: which project's TSDB to query (multi-tenant)
};

// ─── Per-rule runtime state ───
struct AlertRuleState {
    AlertRuleConfig rule;
    AlertState      state{AlertState::INACTIVE};
    int64_t         last_fired_ns{0};
    int64_t         cooldown_until_ns{0};
    std::atomic<uint64_t> fire_count{0};

    AlertRuleState() = default;
    explicit AlertRuleState(AlertRuleConfig r) : rule(std::move(r)) {}

    // Non-copyable due to atomic
    AlertRuleState(const AlertRuleState&) = delete;
    AlertRuleState& operator=(const AlertRuleState&) = delete;
    AlertRuleState(AlertRuleState&&) = delete;
    AlertRuleState& operator=(AlertRuleState&&) = delete;
};

// ─── A fired alert (also used by AlertDelivery in Phase 4) ───
struct FiredAlert {
    std::string rule_id;
    std::string app;
    std::string level;
    std::string message;     // Human-readable summary
    std::string notify_url;
    int         count{0};
    int         threshold{0};
    int         window_seconds{0};
    int64_t     fired_at_ns{0};
    bool        delivered{false};
    std::string delivery_error;
};

// ─── AlertRulesEngine ───

class AlertRulesEngine {
public:
    static constexpr int CHECK_INTERVAL_S = 10;
    static constexpr int MAX_HISTORY      = 10;  // for Dashboard panel

    /// Single-tenant constructor (used by existing tests — unchanged behaviour).
    AlertRulesEngine(TSDB& tsdb,
                     AlertDelivery* delivery,
                     std::string rules_path,
                     std::atomic<bool>& stop_flag)
        : tsdb_ptr_(&tsdb)
        , tenant_mgr_(nullptr)
        , delivery_(delivery)
        , rules_path_(std::move(rules_path))
        , stop_flag_(stop_flag)
    {
        load_rules();
    }

    /// Multi-tenant constructor — resolves TSDB per rule via TenantManager.
    AlertRulesEngine(TenantManager& tenant_mgr,
                     AlertDelivery* delivery,
                     std::string rules_path,
                     std::atomic<bool>& stop_flag)
        : tsdb_ptr_(nullptr)
        , tenant_mgr_(&tenant_mgr)
        , delivery_(delivery)
        , rules_path_(std::move(rules_path))
        , stop_flag_(stop_flag)
    {
        load_rules();
    }

    ~AlertRulesEngine() { stop(); }

    void start() {
        check_thread_ = std::thread([this]{ check_loop();     });
        watch_thread_ = std::thread([this]{ file_watch_loop();});
    }

    void stop() {
        if (check_thread_.joinable()) check_thread_.join();
        if (watch_thread_.joinable()) watch_thread_.join();
    }

    void reload() {
        reload_pending_.store(true, std::memory_order_relaxed);
    }

    // ─── For GET /rules ───
    std::string get_rules_json() const {
        std::lock_guard<std::mutex> lock(rules_mutex_);
        nlohmann::json arr = nlohmann::json::array();
        for (const auto& rs : rules_) {
            nlohmann::json obj;
            obj["id"]               = rs->rule.id;
            obj["app"]              = rs->rule.app;
            obj["level"]            = rs->rule.level;
            obj["threshold"]        = rs->rule.threshold;
            obj["window_seconds"]   = rs->rule.window_seconds;
            obj["notify_url"]       = rs->rule.notify_url;
            obj["cooldown_seconds"] = rs->rule.cooldown_seconds;
            obj["state"]            = alert_state_to_string(rs->state);
            obj["fire_count"]       = rs->fire_count.load(std::memory_order_relaxed);
            obj["last_fired_ns"]    = rs->last_fired_ns;
            arr.push_back(std::move(obj));
        }
        nlohmann::json result;
        result["rules"] = arr;
        return result.dump();
    }

    // ─── For Dashboard (Phase 6) ───
    std::vector<FiredAlert> get_fired_history() const {
        std::lock_guard<std::mutex> lock(history_mutex_);
        return std::vector<FiredAlert>(fired_history_.begin(), fired_history_.end());
    }

private:
    // ─── Evaluation loop ───

    void check_loop() {
        int ticks = 0;
        while (!stop_flag_.load(std::memory_order_relaxed)) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            if (++ticks < CHECK_INTERVAL_S) continue;
            ticks = 0;

            // Hot reload if pending
            if (reload_pending_.exchange(false, std::memory_order_acq_rel)) {
                load_rules();
            }

            std::lock_guard<std::mutex> lock(rules_mutex_);
            for (auto& rs : rules_) {
                evaluate_rule(*rs);
            }
        }
    }

    void evaluate_rule(AlertRuleState& rs) {
        int64_t now = now_ns();

        // Transition COOLDOWN → INACTIVE if expired
        if (rs.state == AlertState::COOLDOWN &&
            now >= rs.cooldown_until_ns) {
            rs.state = AlertState::INACTIVE;
        }

        // Skip if already in cooldown
        if (rs.state == AlertState::COOLDOWN) return;

        // Resolve which TSDB to query
        TSDB* tsdb = tsdb_ptr_;
        if (tenant_mgr_) {
            tsdb = rs.rule.project_id.empty()
                     ? tenant_mgr_->get_default_tsdb()
                     : tenant_mgr_->get_tsdb(rs.rule.project_id);
        }
        if (!tsdb) return;  // no TSDB available for this rule

        int count = tsdb->getCount(rs.rule.app, rs.rule.level,
                                   static_cast<int64_t>(rs.rule.window_seconds));

        if (count > rs.rule.threshold) {
            fire_alert(rs, count, now);
        } else {
            if (rs.state == AlertState::FIRING) {
                rs.state = AlertState::INACTIVE;
            }
        }
    }

    void fire_alert(AlertRuleState& rs, int count, int64_t now) {
        rs.state          = AlertState::FIRING;
        rs.last_fired_ns  = now;
        rs.cooldown_until_ns = now + static_cast<int64_t>(rs.rule.cooldown_seconds)
                                   * 1'000'000'000LL;
        rs.fire_count.fetch_add(1, std::memory_order_relaxed);

        FiredAlert alert;
        alert.rule_id        = rs.rule.id;
        alert.app            = rs.rule.app;
        alert.level          = rs.rule.level;
        alert.count          = count;
        alert.threshold      = rs.rule.threshold;
        alert.window_seconds = rs.rule.window_seconds;
        alert.fired_at_ns    = now;
        alert.notify_url     = rs.rule.notify_url;
        alert.message        = "Alert: " + rs.rule.app + " " + rs.rule.level
                             + " count " + std::to_string(count)
                             + " exceeded threshold " + std::to_string(rs.rule.threshold)
                             + " in " + std::to_string(rs.rule.window_seconds) + "s";

        std::cout << "\033[1;31m[ALERT FIRED] " << alert.message << "\033[0m\n";

        // Store in history for Dashboard
        {
            std::lock_guard<std::mutex> hlock(history_mutex_);
            fired_history_.push_back(alert);
            while (static_cast<int>(fired_history_.size()) > MAX_HISTORY) {
                fired_history_.pop_front();
            }
        }

        // Dispatch to AlertDelivery (Phase 4)
        if (delivery_) {
            dispatch_alert(alert);
        }

        // Transition to COOLDOWN after firing
        rs.state = AlertState::COOLDOWN;
    }

    void dispatch_alert(const FiredAlert& alert);  // Defined after AlertDelivery

    // ─── Rule loading ───

    void load_rules() {
        try {
            std::ifstream f(rules_path_);
            if (!f.is_open()) {
                std::cerr << "[AlertRulesEngine] Cannot open: " << rules_path_ << "\n";
                return;
            }
            nlohmann::json arr = nlohmann::json::parse(f);
            if (!arr.is_array()) {
                std::cerr << "[AlertRulesEngine] alerts.json must be a JSON array\n";
                return;
            }

            // Build new rules, preserving cooldown state for matching IDs
            std::unordered_map<std::string, const AlertRuleState*> prev_states;
            {
                std::lock_guard<std::mutex> lock(rules_mutex_);
                for (const auto& rs : rules_) {
                    prev_states[rs->rule.id] = rs.get();
                }
            }

            std::vector<std::unique_ptr<AlertRuleState>> new_rules;
            for (const auto& item : arr) {
                AlertRuleConfig cfg;
                cfg.id               = item.value("id", "");
                cfg.app              = item.value("app", "");
                cfg.level            = item.value("level", "ERROR");
                cfg.threshold        = item.value("threshold", 5);
                cfg.window_seconds   = item.value("window_seconds", 60);
                cfg.notify_url       = item.value("notify_url", "");
                cfg.cooldown_seconds = item.value("cooldown_seconds", 300);
                cfg.project_id       = item.value("project_id", "");

                if (cfg.id.empty() || cfg.app.empty()) continue;

                auto rs = std::make_unique<AlertRuleState>(cfg);

                // Restore cooldown state if rule ID was already present
                auto it = prev_states.find(cfg.id);
                if (it != prev_states.end()) {
                    rs->state            = it->second->state;
                    rs->last_fired_ns    = it->second->last_fired_ns;
                    rs->cooldown_until_ns= it->second->cooldown_until_ns;
                    rs->fire_count.store(it->second->fire_count.load());
                }
                new_rules.push_back(std::move(rs));
            }

            {
                std::lock_guard<std::mutex> lock(rules_mutex_);
                rules_ = std::move(new_rules);
            }
            std::cout << "[AlertRulesEngine] Loaded " << new_rules.size()
                      << " rules from " << rules_path_ << "\n";
            // new_rules is empty after move; use rules_ size
            {
                std::lock_guard<std::mutex> lock(rules_mutex_);
                std::cout << "[AlertRulesEngine] " << rules_.size()
                          << " rules active\n";
            }
        } catch (const std::exception& ex) {
            std::cerr << "[AlertRulesEngine] Failed to load rules: "
                      << ex.what() << "\n";
        }
    }

    // ─── Hot-reload file watcher ───

    void file_watch_loop() {
#ifdef __linux__
        file_watch_inotify();
#elif defined(__APPLE__)
        file_watch_kqueue();
#else
        file_watch_polling();
#endif
    }

#ifdef __linux__
    void file_watch_inotify() {
        int ifd = inotify_init();
        if (ifd < 0) {
            std::cerr << "[AlertRulesEngine] inotify_init failed\n";
            return;
        }
        int wd = inotify_add_watch(ifd, rules_path_.c_str(),
                                   IN_CLOSE_WRITE | IN_MOVED_TO);
        if (wd < 0) {
            close(ifd);
            file_watch_polling();
            return;
        }

        char buf[1024];
        while (!stop_flag_.load(std::memory_order_relaxed)) {
            fd_set rfds;
            FD_ZERO(&rfds);
            FD_SET(ifd, &rfds);
            struct timeval tv{1, 0};  // 1s timeout for stop_flag check
            int ret = select(ifd + 1, &rfds, nullptr, nullptr, &tv);
            if (ret > 0 && FD_ISSET(ifd, &rfds)) {
                ssize_t len = read(ifd, buf, sizeof(buf));
                if (len > 0) {
                    reload_pending_.store(true, std::memory_order_relaxed);
                }
            }
        }
        inotify_rm_watch(ifd, wd);
        close(ifd);
    }
#endif

#if defined(__APPLE__)
    void file_watch_kqueue() {
        int kq = kqueue();
        if (kq < 0) {
            file_watch_polling();
            return;
        }
        int fd = open(rules_path_.c_str(), O_RDONLY | O_EVTONLY);
        if (fd < 0) {
            close(kq);
            file_watch_polling();
            return;
        }

        struct kevent change;
        EV_SET(&change, static_cast<uintptr_t>(fd), EVFILT_VNODE,
               EV_ADD | EV_ENABLE | EV_CLEAR,
               NOTE_WRITE | NOTE_EXTEND | NOTE_RENAME, 0, nullptr);
        kevent(kq, &change, 1, nullptr, 0, nullptr);

        struct kevent event;
        while (!stop_flag_.load(std::memory_order_relaxed)) {
            struct timespec timeout{1, 0};  // 1s for stop_flag check
            int nev = kevent(kq, nullptr, 0, &event, 1, &timeout);
            if (nev > 0) {
                reload_pending_.store(true, std::memory_order_relaxed);
            }
        }
        close(fd);
        close(kq);
    }
#endif

    void file_watch_polling() {
        // Fallback: poll file modification time every 5 seconds
        auto last_mtime = std::filesystem::last_write_time(
            std::filesystem::path(rules_path_));
        while (!stop_flag_.load(std::memory_order_relaxed)) {
            std::this_thread::sleep_for(std::chrono::seconds(5));
            std::error_code ec;
            auto mtime = std::filesystem::last_write_time(
                std::filesystem::path(rules_path_), ec);
            if (!ec && mtime != last_mtime) {
                last_mtime = mtime;
                reload_pending_.store(true, std::memory_order_relaxed);
            }
        }
    }

    static int64_t now_ns() {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
    }

    // ─── Members ───
    TSDB*           tsdb_ptr_{nullptr};    // set by single-tenant constructor
    TenantManager*  tenant_mgr_{nullptr};  // set by multi-tenant constructor
    AlertDelivery* delivery_;
    std::string rules_path_;
    std::atomic<bool>& stop_flag_;

    mutable std::mutex rules_mutex_;
    std::vector<std::unique_ptr<AlertRuleState>> rules_;

    mutable std::mutex history_mutex_;
    std::deque<FiredAlert> fired_history_;

    std::atomic<bool> reload_pending_{false};
    std::thread check_thread_;
    std::thread watch_thread_;
};

} // namespace logmonitor

// ─── AlertDelivery dispatch stub ───
// Phase 4 provides the real implementation by defining LOGMONITOR_ALERT_DELIVERY_INCLUDED
// before including this header, which replaces this stub.
#ifndef LOGMONITOR_ALERT_DELIVERY_INCLUDED
namespace logmonitor {
    inline void AlertRulesEngine::dispatch_alert(const FiredAlert& /*alert*/) {
        // No-op until Phase 4 wires in AlertDelivery
    }
}
#endif
