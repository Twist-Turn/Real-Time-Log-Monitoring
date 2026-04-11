/**
 * @file UserStore.hpp
 * @brief User account storage with JSON persistence and PBKDF2 password hashing.
 *
 * Thread-safe via shared_mutex (multiple concurrent readers, exclusive writers).
 * Persists to data/users.json using atomic rename to prevent corruption.
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

// ─── User model ──────────────────────────────────────────────────────────────

struct User {
    std::string id;             // random_hex(8)
    std::string email;
    std::string password_hash;  // hex PBKDF2-SHA256 output
    std::string salt;           // hex 16-byte random salt
    int64_t     created_at_ns{0};
};

// ─── UserStore ───────────────────────────────────────────────────────────────

class UserStore {
public:
    explicit UserStore(std::string path)
        : persistence_path_(std::move(path))
    {
        namespace fs = std::filesystem;
        fs::create_directories(fs::path(persistence_path_).parent_path());
        load();
    }

    /**
     * Register a new user.
     * Returns "" on success (out_user is populated).
     * Returns an error string on failure (duplicate email, etc.).
     */
    std::string register_user(const std::string& email,
                              const std::string& password,
                              User& out_user) {
        if (email.empty() || password.empty())
            return "Email and password are required";
        if (password.size() < 8)
            return "Password must be at least 8 characters";

        std::unique_lock<std::shared_mutex> lk(rw_mutex_);

        if (by_email_.count(email))
            return "Email already registered";

        User u;
        u.id            = gen_user_id();
        u.email         = email;
        u.salt          = AuthManager::generate_salt();
        u.password_hash = AuthManager::hash_password(password, u.salt);
        u.created_at_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();

        by_email_[u.email] = users_.size();
        by_id_[u.id]       = users_.size();
        users_.push_back(u);

        save_locked();
        out_user = u;
        return "";
    }

    bool find_by_email(const std::string& email, User& out) const {
        std::shared_lock<std::shared_mutex> lk(rw_mutex_);
        auto it = by_email_.find(email);
        if (it == by_email_.end()) return false;
        out = users_[it->second];
        return true;
    }

    bool find_by_id(const std::string& id, User& out) const {
        std::shared_lock<std::shared_mutex> lk(rw_mutex_);
        auto it = by_id_.find(id);
        if (it == by_id_.end()) return false;
        out = users_[it->second];
        return true;
    }

    std::size_t size() const {
        std::shared_lock<std::shared_mutex> lk(rw_mutex_);
        return users_.size();
    }

private:
    void load() {
        std::ifstream f(persistence_path_);
        if (!f.is_open()) return;

        nlohmann::json j;
        try { j = nlohmann::json::parse(f); }
        catch (...) { return; }

        if (!j.is_array()) return;

        users_.clear();
        by_email_.clear();
        by_id_.clear();

        for (auto& obj : j) {
            User u;
            u.id            = obj.value("id",            "");
            u.email         = obj.value("email",         "");
            u.password_hash = obj.value("password_hash", "");
            u.salt          = obj.value("salt",          "");
            u.created_at_ns = obj.value("created_at_ns", int64_t{0});
            if (u.id.empty() || u.email.empty()) continue;
            by_email_[u.email] = users_.size();
            by_id_[u.id]       = users_.size();
            users_.push_back(std::move(u));
        }
    }

    // Must be called with unique_lock held
    void save_locked() const {
        nlohmann::json arr = nlohmann::json::array();
        for (const auto& u : users_) {
            arr.push_back({
                {"id",            u.id},
                {"email",         u.email},
                {"password_hash", u.password_hash},
                {"salt",          u.salt},
                {"created_at_ns", u.created_at_ns}
            });
        }
        std::string tmp = persistence_path_ + ".tmp";
        std::ofstream f(tmp);
        f << arr.dump(2);
        f.close();
        std::filesystem::rename(tmp, persistence_path_);
    }

    static std::string gen_user_id() {
        return AuthManager::random_hex(8); // 16 hex chars
    }

    std::string persistence_path_;
    mutable std::shared_mutex rw_mutex_;
    std::vector<User> users_;
    std::unordered_map<std::string, std::size_t> by_email_;
    std::unordered_map<std::string, std::size_t> by_id_;
};

} // namespace logmonitor
