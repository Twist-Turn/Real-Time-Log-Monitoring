/**
 * @file ProjectStore.hpp
 * @brief Project management: CRUD, per-project API key generation and O(1) lookup.
 *
 * API key format: "lm_proj_" + random_hex(16)  (40 chars total)
 * Thread-safe via shared_mutex.
 * Persists to data/projects.json with atomic rename.
 */
#pragma once

#include "AuthManager.hpp"

#include <nlohmann/json.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace logmonitor {

// ─── Project model ───────────────────────────────────────────────────────────

struct Project {
    std::string id;           // random_hex(8) — 16 chars
    std::string name;
    std::string owner_id;     // User.id
    std::string api_key;      // "lm_proj_" + random_hex(16)
    int64_t     created_at_ns{0};
};

// ─── ProjectStore ────────────────────────────────────────────────────────────

class ProjectStore {
public:
    explicit ProjectStore(std::string path)
        : persistence_path_(std::move(path))
    {
        namespace fs = std::filesystem;
        fs::create_directories(fs::path(persistence_path_).parent_path());
        load();
    }

    /**
     * Create a new project for owner_id.
     * Returns "" on success (out_project populated with id and api_key).
     */
    std::string create_project(const std::string& name,
                               const std::string& owner_id,
                               Project& out_project) {
        if (name.empty())    return "Project name is required";
        if (owner_id.empty()) return "Owner ID is required";

        std::unique_lock<std::shared_mutex> lk(rw_mutex_);

        Project p;
        p.id            = gen_project_id();
        p.name          = name;
        p.owner_id      = owner_id;
        p.api_key       = "lm_proj_" + AuthManager::random_hex(16);
        p.created_at_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();

        std::size_t idx = projects_.size();
        by_id_[p.id]          = idx;
        by_api_key_[p.api_key] = idx;
        by_owner_[p.owner_id].push_back(idx);
        projects_.push_back(p);

        save_locked();
        out_project = p;
        return "";
    }

    bool find_by_id(const std::string& id, Project& out) const {
        std::shared_lock<std::shared_mutex> lk(rw_mutex_);
        auto it = by_id_.find(id);
        if (it == by_id_.end()) return false;
        out = projects_[it->second];
        return true;
    }

    /// O(1) — called on every /ingest request.
    bool find_by_api_key(const std::string& key, Project& out) const {
        std::shared_lock<std::shared_mutex> lk(rw_mutex_);
        auto it = by_api_key_.find(key);
        if (it == by_api_key_.end()) return false;
        out = projects_[it->second];
        return true;
    }

    std::vector<Project> list_by_owner(const std::string& owner_id) const {
        std::shared_lock<std::shared_mutex> lk(rw_mutex_);
        std::vector<Project> out;
        auto it = by_owner_.find(owner_id);
        if (it == by_owner_.end()) return out;
        for (auto idx : it->second)
            out.push_back(projects_[idx]);
        return out;
    }

    /**
     * Delete project by id. Verifies requesting_owner_id matches.
     * Returns "" on success, error string on failure.
     */
    std::string delete_project(const std::string& id,
                               const std::string& requesting_owner_id) {
        std::unique_lock<std::shared_mutex> lk(rw_mutex_);

        auto it = by_id_.find(id);
        if (it == by_id_.end()) return "Project not found";

        std::size_t idx = it->second;
        const Project& p = projects_[idx];

        if (p.owner_id != requesting_owner_id)
            return "Not your project";

        // Remove from index maps
        by_api_key_.erase(p.api_key);
        auto& owner_list = by_owner_[p.owner_id];
        owner_list.erase(
            std::remove(owner_list.begin(), owner_list.end(), idx),
            owner_list.end());
        by_id_.erase(it);

        // Mark as deleted (set empty id so we can skip on reload)
        projects_[idx].id = "";

        save_locked();
        return "";
    }

    /**
     * Rotate the project's API key.
     * Returns the new api_key on success, "" on failure.
     */
    std::string rotate_api_key(const std::string& id,
                               const std::string& requesting_owner_id) {
        std::unique_lock<std::shared_mutex> lk(rw_mutex_);

        auto it = by_id_.find(id);
        if (it == by_id_.end()) return "";

        std::size_t idx = it->second;
        Project& p = projects_[idx];

        if (p.owner_id != requesting_owner_id) return "";

        // Erase old key from index
        by_api_key_.erase(p.api_key);

        // Generate new key
        p.api_key = "lm_proj_" + AuthManager::random_hex(16);
        by_api_key_[p.api_key] = idx;

        save_locked();
        return p.api_key;
    }

    std::size_t size() const {
        std::shared_lock<std::shared_mutex> lk(rw_mutex_);
        std::size_t cnt = 0;
        for (const auto& p : projects_) if (!p.id.empty()) ++cnt;
        return cnt;
    }

private:
    void load() {
        std::ifstream f(persistence_path_);
        if (!f.is_open()) return;

        nlohmann::json j;
        try { j = nlohmann::json::parse(f); }
        catch (...) { return; }

        if (!j.is_array()) return;

        projects_.clear();
        by_id_.clear();
        by_api_key_.clear();
        by_owner_.clear();

        for (auto& obj : j) {
            Project p;
            p.id            = obj.value("id",            "");
            p.name          = obj.value("name",          "");
            p.owner_id      = obj.value("owner_id",      "");
            p.api_key       = obj.value("api_key",       "");
            p.created_at_ns = obj.value("created_at_ns", int64_t{0});
            if (p.id.empty() || p.api_key.empty()) continue; // deleted
            std::size_t idx = projects_.size();
            by_id_[p.id]          = idx;
            by_api_key_[p.api_key] = idx;
            by_owner_[p.owner_id].push_back(idx);
            projects_.push_back(std::move(p));
        }
    }

    void save_locked() const {
        nlohmann::json arr = nlohmann::json::array();
        for (const auto& p : projects_) {
            if (p.id.empty()) continue; // skip deleted
            arr.push_back({
                {"id",            p.id},
                {"name",          p.name},
                {"owner_id",      p.owner_id},
                {"api_key",       p.api_key},
                {"created_at_ns", p.created_at_ns}
            });
        }
        std::string tmp = persistence_path_ + ".tmp";
        std::ofstream f(tmp);
        f << arr.dump(2);
        f.close();
        std::filesystem::rename(tmp, persistence_path_);
    }

    static std::string gen_project_id() {
        return AuthManager::random_hex(8); // 16 hex chars
    }

    std::string persistence_path_;
    mutable std::shared_mutex rw_mutex_;
    std::vector<Project> projects_;
    std::unordered_map<std::string, std::size_t> by_id_;
    std::unordered_map<std::string, std::size_t> by_api_key_;
    std::unordered_map<std::string, std::vector<std::size_t>> by_owner_;
};

} // namespace logmonitor
