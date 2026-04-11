/**
 * @file TenantManager.hpp
 * @brief Routes log ingestion to per-project TSDB instances.
 *
 * Each project gets its own isolated TSDB, keyed by project_id.
 * TSDbs are created lazily on first ingest and persisted to
 * data/{project_id}.bin.
 *
 * Thread-safe: two-phase lock (shared for lookup, exclusive for creation).
 */
#pragma once

#include "ProjectStore.hpp"
#include "TSDB.hpp"

#include <atomic>
#include <memory>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace logmonitor {

class TenantManager {
public:
    TenantManager(ProjectStore& project_store,
                  std::string   data_dir,
                  std::atomic<bool>& stop_flag,
                  std::string   default_project_id = "")
        : project_store_(project_store)
        , data_dir_(std::move(data_dir))
        , stop_flag_(stop_flag)
        , default_project_id_(std::move(default_project_id))
    {
        namespace fs = std::filesystem;
        fs::create_directories(data_dir_);

        // Pre-warm TSDbs for all existing projects so they load persisted data
        if (!default_project_id_.empty()) {
            Project dummy;
            if (project_store_.find_by_id(default_project_id_, dummy))
                get_or_create_tsdb(default_project_id_);
        }
    }

    ~TenantManager() { stop_all(); }

    // ─── Ingest routing ─────────────────────────────────────────────────────

    /**
     * Look up project by API key, insert entry into its TSDB.
     * Returns false if the api_key is not recognized.
     */
    bool route_ingest(const std::string& api_key,
                      const std::string& app,
                      const std::string& level,
                      const std::string& message,
                      int64_t            timestamp_ns) {
        Project proj;
        if (!project_store_.find_by_api_key(api_key, proj)) return false;

        ingest_to_project(proj.id, app, level, message, timestamp_ns);
        return true;
    }

    /**
     * Insert directly by project_id (used by pipeline callback for file/TCP sources).
     */
    void ingest_to_project(const std::string& project_id,
                           const std::string& app,
                           const std::string& level,
                           const std::string& message,
                           int64_t            timestamp_ns) {
        if (project_id.empty()) return;
        auto& tsdb = get_or_create_tsdb(project_id);
        tsdb.insert({app, level, message, timestamp_ns});
    }

    // ─── TSDB access ────────────────────────────────────────────────────────

    /**
     * Returns the TSDB for project_id (creates lazily).
     * Returns nullptr if project_id is empty.
     */
    TSDB* get_tsdb(const std::string& project_id) {
        if (project_id.empty()) return nullptr;
        return &get_or_create_tsdb(project_id);
    }

    /// Returns the default project's TSDB, or nullptr if not configured.
    TSDB* get_default_tsdb() {
        return get_tsdb(default_project_id_);
    }

    // ─── Lifecycle ──────────────────────────────────────────────────────────

    /// Start all already-loaded TSDbs (idempotent).
    void start_all() {
        std::shared_lock<std::shared_mutex> lk(tsdb_map_mutex_);
        // TSDbs are started in get_or_create_tsdb; this is a no-op
        // but kept for symmetry and future use.
    }

    /// Stop and persist all TSDbs. Safe to call multiple times.
    void stop_all() {
        std::unique_lock<std::shared_mutex> lk(tsdb_map_mutex_);
        for (auto& [id, tsdb] : tsdb_map_) {
            if (tsdb) {
                tsdb->stop();
            }
        }
    }

    // ─── Diagnostics ────────────────────────────────────────────────────────

    std::vector<std::string> active_project_ids() const {
        std::shared_lock<std::shared_mutex> lk(tsdb_map_mutex_);
        std::vector<std::string> ids;
        ids.reserve(tsdb_map_.size());
        for (const auto& [id, _] : tsdb_map_) ids.push_back(id);
        return ids;
    }

    const std::string& default_project_id() const { return default_project_id_; }

private:
    // ─── Two-phase lock: shared read → exclusive create ──────────────────────

    TSDB& get_or_create_tsdb(const std::string& project_id) {
        // Phase 1: shared read lock — fast path for existing projects
        {
            std::shared_lock<std::shared_mutex> lk(tsdb_map_mutex_);
            auto it = tsdb_map_.find(project_id);
            if (it != tsdb_map_.end() && it->second) return *it->second;
        }

        // Phase 2: exclusive write lock — create new TSDB
        std::unique_lock<std::shared_mutex> lk(tsdb_map_mutex_);
        // Double-check: another thread may have created it while we waited
        auto it = tsdb_map_.find(project_id);
        if (it != tsdb_map_.end() && it->second) return *it->second;

        std::string path = data_dir_ + "/" + project_id + ".bin";
        auto tsdb = std::make_unique<TSDB>(path, stop_flag_);
        tsdb->start();
        TSDB* ptr = tsdb.get();
        tsdb_map_[project_id] = std::move(tsdb);
        return *ptr;
    }

    // ─── Members ────────────────────────────────────────────────────────────
    ProjectStore&      project_store_;
    std::string        data_dir_;
    std::atomic<bool>& stop_flag_;
    std::string        default_project_id_;

    mutable std::shared_mutex tsdb_map_mutex_;
    std::unordered_map<std::string, std::unique_ptr<TSDB>> tsdb_map_;
};

} // namespace logmonitor
