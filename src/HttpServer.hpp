#pragma once
/**
 * @file HttpServer.hpp
 * @brief Production HTTP server built on cpp-httplib, listening on port 9090.
 *
 * Single-tenant legacy endpoints (backward compat, X-API-Key auth):
 *   POST /ingest, GET /health, /query, /stats, /apps, /metrics, /rules, /alerts/history
 *
 * Multi-tenant SaaS endpoints (JWT Bearer auth — active when auth_mgr_ != nullptr):
 *   GET  /                          Login/register page (HTML)
 *   GET  /app                       Dashboard shell (HTML)
 *   GET  /static/app.{js,css}       Static assets
 *   POST /auth/register             Create account
 *   POST /auth/login                Get JWT token
 *   GET/POST/DELETE /api/projects   Project CRUD
 *   POST /api/projects/{id}/rotate-key
 *   POST /ingest                    (updated: routes by project API key)
 *   GET  /api/projects/{id}/query|stats|apps|metrics|rules|alerts/history
 */

// Suppress warnings from third-party headers
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
#pragma GCC diagnostic ignored "-Wold-style-cast"
#pragma GCC diagnostic ignored "-Wshadow"
#pragma GCC diagnostic ignored "-Wconversion"
#pragma GCC diagnostic ignored "-Wformat-nonliteral"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#include <httplib.h>
#pragma GCC diagnostic pop

#include "AuthManager.hpp"
#include "FileWatcher.hpp"   // LogRingBuffer
#include "PatternEngine.hpp" // LogEntry
#include "ProjectStore.hpp"
#include "TenantManager.hpp"
#include "UserStore.hpp"
#include "WebUI.hpp"

#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <ctime>
#include <functional>
#include <iostream>
#include <mutex>
#include <shared_mutex>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>

namespace logmonitor {

// ─── ANSI color codes ──────────────────────────────────────────────────────
namespace ansi {
    constexpr const char* RESET    = "\033[0m";
    constexpr const char* RED      = "\033[31m";
    constexpr const char* YELLOW   = "\033[33m";
    constexpr const char* GREEN    = "\033[32m";
    constexpr const char* BOLD_RED = "\033[1;31m";
    constexpr const char* CYAN     = "\033[36m";
}

// ─── Token Bucket (per-key rate limiting) ─────────────────────────────────
struct TokenBucket {
    double  tokens{100.0};
    int64_t last_refill_ns{0};
    static constexpr double MAX_TOKENS        = 100.0;
    static constexpr double REFILL_PER_SECOND = 100.0 / 60.0; // 100 req/min

    bool consume() {
        auto now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        if (last_refill_ns > 0) {
            double elapsed_s = static_cast<double>(now_ns - last_refill_ns) / 1e9;
            tokens = std::min(MAX_TOKENS, tokens + elapsed_s * REFILL_PER_SECOND);
        }
        last_refill_ns = now_ns;
        if (tokens < 1.0) return false;
        tokens -= 1.0;
        return true;
    }
};

// ─── Callback types (single-tenant backward compat) ───────────────────────
using InsertCb  = std::function<void(const std::string&, const std::string&,
                                     const std::string&, int64_t)>;
using QueryCb   = std::function<std::string(const std::string&,
                                             const std::string&, int)>;
using StatsCb   = std::function<std::string(const std::string&, int)>;
using AppsCb    = std::function<std::string()>;
using MetricsCb = std::function<std::string()>;
using RulesCb   = std::function<std::string()>;
using HistoryCb = std::function<std::string()>;

// ─── HttpServer ───────────────────────────────────────────────────────────

class HttpServer {
public:
    /**
     * Constructor.
     *
     * The last 4 parameters are all nullptr by default — existing callers
     * passing 5 arguments continue to work unchanged (backward compatible).
     *
     * When auth_mgr_ != nullptr, multi-tenant SaaS routes are activated.
     */
    HttpServer(LogRingBuffer&     ring_buffer,
               uint16_t           port,
               std::string        api_key,
               std::atomic<bool>& stop_flag,
               std::size_t        thread_count  = 10,
               TenantManager*     tenant_mgr    = nullptr,
               AuthManager*       auth_mgr      = nullptr,
               UserStore*         user_store    = nullptr,
               ProjectStore*      project_store = nullptr)
        : ring_buffer_(ring_buffer)
        , port_(port)
        , api_key_(std::move(api_key))
        , stop_flag_(stop_flag)
        , tenant_mgr_(tenant_mgr)
        , auth_mgr_(auth_mgr)
        , user_store_(user_store)
        , project_store_(project_store)
    {
        svr_.new_task_queue = [thread_count]() {
            return new httplib::ThreadPool(thread_count);
        };
        svr_.set_payload_max_length(1 * 1024 * 1024); // 1 MB
        setup_routes();
    }

    ~HttpServer() { stop(); }

    // ─── Legacy callback setters ────────────────────────────────────────────
    void set_insert_cb(InsertCb  cb) { insert_cb_  = std::move(cb); }
    void set_query_cb(QueryCb    cb) { query_cb_   = std::move(cb); }
    void set_stats_cb(StatsCb    cb) { stats_cb_   = std::move(cb); }
    void set_apps_cb(AppsCb      cb) { apps_cb_    = std::move(cb); }
    void set_metrics_cb(MetricsCb cb){ metrics_cb_ = std::move(cb); }
    void set_rules_cb(RulesCb    cb) { rules_cb_   = std::move(cb); }
    void set_history_cb(HistoryCb cb){ history_cb_ = std::move(cb); }

    void start() {
        server_thread_ = std::thread([this]() {
            svr_.listen("0.0.0.0", static_cast<int>(port_));
        });
        std::cout << ansi::CYAN << "[HttpServer] Listening on 0.0.0.0:"
                  << port_ << ansi::RESET << "\n";
    }

    void stop() {
        svr_.stop();
        if (server_thread_.joinable()) server_thread_.join();
    }

    [[nodiscard]] uint64_t total_ingested() const {
        return total_ingested_.load(std::memory_order_relaxed);
    }

private:
    // ─── Route registration ──────────────────────────────────────────────────
    void setup_routes() {
        using Req = const httplib::Request&;
        using Res = httplib::Response&;

        // ── Static / Web UI (no auth) ──
        svr_.Get("/",                [this](Req q, Res r){ handle_root(q, r);      });
        svr_.Get("/app",             [this](Req q, Res r){ handle_app(q, r);       });
        svr_.Get("/static/app.css",  [this](Req q, Res r){ handle_css(q, r);       });
        svr_.Get("/static/app.js",   [this](Req q, Res r){ handle_js(q, r);        });

        // ── Health (no auth) ──
        svr_.Get("/health",          [this](Req q, Res r){ handle_health(q, r);    });

        // ── Auth routes (no auth) ──
        svr_.Post("/auth/register",  [this](Req q, Res r){ handle_register(q, r);  });
        svr_.Post("/auth/login",     [this](Req q, Res r){ handle_login(q, r);     });

        // ── Project management (JWT Bearer) ──
        // NOTE: more-specific patterns registered BEFORE general ones
        svr_.Post(R"(/api/projects/([^/]+)/rotate-key)",
                                     [this](Req q, Res r){ handle_proj_rotate(q, r); });
        svr_.Get(R"(/api/projects/([^/]+)/query)",
                                     [this](Req q, Res r){ handle_proj_query(q, r);  });
        svr_.Get(R"(/api/projects/([^/]+)/stats)",
                                     [this](Req q, Res r){ handle_proj_stats(q, r);  });
        svr_.Get(R"(/api/projects/([^/]+)/apps)",
                                     [this](Req q, Res r){ handle_proj_apps(q, r);   });
        svr_.Get(R"(/api/projects/([^/]+)/metrics)",
                                     [this](Req q, Res r){ handle_proj_metrics(q, r);});
        svr_.Get(R"(/api/projects/([^/]+)/rules)",
                                     [this](Req q, Res r){ handle_proj_rules(q, r);  });
        svr_.Get(R"(/api/projects/([^/]+)/alerts/history)",
                                     [this](Req q, Res r){ handle_proj_history(q, r);});
        svr_.Get("/api/projects",    [this](Req q, Res r){ handle_proj_list(q, r);   });
        svr_.Post("/api/projects",   [this](Req q, Res r){ handle_proj_create(q, r); });
        svr_.Delete(R"(/api/projects/([^/]+))",
                                     [this](Req q, Res r){ handle_proj_delete(q, r); });

        // ── Log ingest (project API key in multi-tenant, X-API-Key in legacy) ──
        svr_.Post("/ingest",         [this](Req q, Res r){ handle_ingest(q, r);    });

        // ── Legacy single-tenant routes (X-API-Key) ──
        svr_.Get("/query",           [this](Req q, Res r){ handle_query(q, r);     });
        svr_.Get("/stats",           [this](Req q, Res r){ handle_stats(q, r);     });
        svr_.Get("/apps",            [this](Req q, Res r){ handle_apps(q, r);      });
        svr_.Get("/metrics",         [this](Req q, Res r){ handle_metrics(q, r);   });
        svr_.Get("/rules",           [this](Req q, Res r){ handle_rules(q, r);     });
        svr_.Get("/alerts/history",  [this](Req q, Res r){ handle_history(q, r);   });

        svr_.set_pre_routing_handler([this](Req /*q*/, Res /*r*/) {
            if (stop_flag_.load(std::memory_order_relaxed)) svr_.stop();
            return httplib::Server::HandlerResponse::Unhandled;
        });
    }

    // ─── Auth helpers ────────────────────────────────────────────────────────

    /**
     * Legacy X-API-Key auth (used by /ingest when no tenant_mgr_, and all
     * single-tenant endpoints).  Returns true if request is authorized.
     */
    bool authenticate(const httplib::Request& req, httplib::Response& res) {
        auto key = req.get_header_value("X-API-Key");
        bool valid = (key.size() == api_key_.size()) &&
                     std::equal(key.begin(), key.end(), api_key_.begin());
        if (!valid) {
            json_error(res, 401, "Unauthorized: invalid or missing X-API-Key");
            return false;
        }
        std::unique_lock<std::shared_mutex> lk(rate_mutex_);
        if (!rate_buckets_[key].consume()) {
            json_error(res, 429, "Rate limit exceeded: 100 requests per minute");
            return false;
        }
        return true;
    }

    /**
     * JWT Bearer auth for /api/... routes.
     * Returns true on success; sets res on failure.
     */
    bool auth_bearer(const httplib::Request& req, httplib::Response& res,
                     JwtClaims& out_claims) {
        if (!auth_mgr_) {
            json_error(res, 503, "Authentication not configured");
            return false;
        }
        auto auth_header = req.get_header_value("Authorization");
        if (auth_header.rfind("Bearer ", 0) != 0) {
            json_error(res, 401, "Missing or invalid Authorization header");
            return false;
        }
        std::string token = auth_header.substr(7);
        if (!auth_mgr_->verify_token(token, out_claims)) {
            json_error(res, 401, "Invalid or expired token");
            return false;
        }
        return true;
    }

    /**
     * Verify that the requesting user owns project_id.
     * Requires auth_bearer to have already been called.
     */
    bool verify_ownership(const std::string& project_id,
                          const JwtClaims& claims,
                          httplib::Response& res) {
        if (!project_store_) {
            json_error(res, 503, "Project store not configured");
            return false;
        }
        Project proj;
        if (!project_store_->find_by_id(project_id, proj)) {
            json_error(res, 404, "Project not found");
            return false;
        }
        if (proj.owner_id != claims.sub) {
            json_error(res, 403, "Not your project");
            return false;
        }
        return true;
    }

    // ─── Static page handlers ────────────────────────────────────────────────

    void handle_root(const httplib::Request&, httplib::Response& res) {
        res.set_content(webui::login_html(), "text/html; charset=utf-8");
    }
    void handle_app(const httplib::Request&, httplib::Response& res) {
        res.set_content(webui::app_html(), "text/html; charset=utf-8");
    }
    void handle_css(const httplib::Request&, httplib::Response& res) {
        res.set_content(webui::app_css(), "text/css; charset=utf-8");
    }
    void handle_js(const httplib::Request&, httplib::Response& res) {
        res.set_content(webui::app_js(), "application/javascript; charset=utf-8");
    }

    // ─── Health ──────────────────────────────────────────────────────────────
    void handle_health(const httplib::Request&, httplib::Response& res) {
        res.set_content(R"({"status":"ok"})", "application/json");
    }

    // ─── POST /auth/register ─────────────────────────────────────────────────
    void handle_register(const httplib::Request& req, httplib::Response& res) {
        if (!auth_mgr_ || !user_store_) {
            json_error(res, 503, "Authentication not configured");
            return;
        }
        nlohmann::json body;
        try { body = nlohmann::json::parse(req.body); }
        catch (...) { json_error(res, 400, "Invalid JSON"); return; }

        std::string email    = body.value("email",    "");
        std::string password = body.value("password", "");
        if (email.empty() || password.empty()) {
            json_error(res, 400, "Missing email or password");
            return;
        }

        User out_user;
        std::string err = user_store_->register_user(email, password, out_user);
        if (!err.empty()) { json_error(res, 400, err); return; }

        nlohmann::json resp;
        resp["user_id"] = out_user.id;
        resp["message"] = "Account created";
        res.set_content(resp.dump(), "application/json");
    }

    // ─── POST /auth/login ─────────────────────────────────────────────────────
    void handle_login(const httplib::Request& req, httplib::Response& res) {
        if (!auth_mgr_ || !user_store_) {
            json_error(res, 503, "Authentication not configured");
            return;
        }
        nlohmann::json body;
        try { body = nlohmann::json::parse(req.body); }
        catch (...) { json_error(res, 400, "Invalid JSON"); return; }

        std::string email    = body.value("email",    "");
        std::string password = body.value("password", "");
        if (email.empty() || password.empty()) {
            json_error(res, 400, "Missing email or password");
            return;
        }

        User user;
        if (!user_store_->find_by_email(email, user)) {
            json_error(res, 401, "Invalid email or password");
            return;
        }
        if (!AuthManager::verify_password(password, user.salt, user.password_hash)) {
            json_error(res, 401, "Invalid email or password");
            return;
        }

        std::string token = auth_mgr_->create_token(user.id, user.email);
        nlohmann::json resp;
        resp["token"]      = token;
        resp["user_id"]    = user.id;
        resp["expires_in"] = AuthManager::TOKEN_EXPIRY_SECONDS;
        res.set_content(resp.dump(), "application/json");
    }

    // ─── GET /api/projects ────────────────────────────────────────────────────
    void handle_proj_list(const httplib::Request& req, httplib::Response& res) {
        JwtClaims claims;
        if (!auth_bearer(req, res, claims)) return;
        if (!project_store_) { json_error(res, 503, "Not configured"); return; }

        auto projs = project_store_->list_by_owner(claims.sub);
        nlohmann::json arr = nlohmann::json::array();
        for (const auto& p : projs) {
            arr.push_back({
                {"id",            p.id},
                {"name",          p.name},
                {"api_key",       p.api_key},
                {"created_at_ns", p.created_at_ns}
            });
        }
        res.set_content(arr.dump(), "application/json");
    }

    // ─── POST /api/projects ───────────────────────────────────────────────────
    void handle_proj_create(const httplib::Request& req, httplib::Response& res) {
        JwtClaims claims;
        if (!auth_bearer(req, res, claims)) return;
        if (!project_store_ || !auth_mgr_) { json_error(res, 503, "Not configured"); return; }

        nlohmann::json body;
        try { body = nlohmann::json::parse(req.body); }
        catch (...) { json_error(res, 400, "Invalid JSON"); return; }

        std::string name = body.value("name", "");
        if (name.empty()) { json_error(res, 400, "Missing name"); return; }

        Project out_proj;
        std::string err = project_store_->create_project(name, claims.sub, out_proj);
        if (!err.empty()) { json_error(res, 400, err); return; }

        nlohmann::json resp = {
            {"id",            out_proj.id},
            {"name",          out_proj.name},
            {"api_key",       out_proj.api_key},
            {"created_at_ns", out_proj.created_at_ns}
        };
        res.set_content(resp.dump(), "application/json");
    }

    // ─── DELETE /api/projects/{id} ────────────────────────────────────────────
    void handle_proj_delete(const httplib::Request& req, httplib::Response& res) {
        JwtClaims claims;
        if (!auth_bearer(req, res, claims)) return;

        std::string project_id = req.matches[1].str();
        if (!verify_ownership(project_id, claims, res)) return;

        std::string err = project_store_->delete_project(project_id, claims.sub);
        if (!err.empty()) { json_error(res, 400, err); return; }

        res.set_content(R"({"message":"Project deleted"})", "application/json");
    }

    // ─── POST /api/projects/{id}/rotate-key ───────────────────────────────────
    void handle_proj_rotate(const httplib::Request& req, httplib::Response& res) {
        JwtClaims claims;
        if (!auth_bearer(req, res, claims)) return;

        std::string project_id = req.matches[1].str();
        if (!verify_ownership(project_id, claims, res)) return;

        std::string new_key = project_store_->rotate_api_key(project_id, claims.sub);
        if (new_key.empty()) { json_error(res, 500, "Failed to rotate key"); return; }

        nlohmann::json resp = {{"api_key", new_key}};
        res.set_content(resp.dump(), "application/json");
    }

    // ─── POST /ingest (updated: routes by project key or falls back) ──────────
    void handle_ingest(const httplib::Request& req, httplib::Response& res) {
        nlohmann::json body;
        try { body = nlohmann::json::parse(req.body); }
        catch (...) { json_error(res, 400, "Invalid JSON body"); return; }

        if (!body.contains("app") || !body.contains("level") || !body.contains("message")) {
            json_error(res, 400, "Missing required fields: app, level, message");
            return;
        }

        const std::string app     = body["app"].get<std::string>();
        const std::string level   = body["level"].get<std::string>();
        const std::string message = body["message"].get<std::string>();
        const int64_t ts_client   = body.value("timestamp", int64_t{0});
        int64_t ts_ns = (ts_client > 0)
            ? ts_client * int64_t{1000000000}
            : std::chrono::duration_cast<std::chrono::nanoseconds>(
                  std::chrono::system_clock::now().time_since_epoch()).count();

        auto api_key_header = req.get_header_value("X-API-Key");

        if (tenant_mgr_) {
            // Multi-tenant path: route by project API key
            if (api_key_header.empty()) {
                json_error(res, 401, "Unauthorized: missing X-API-Key");
                return;
            }
            // Rate limit per project key
            {
                std::unique_lock<std::shared_mutex> lk(rate_mutex_);
                if (!rate_buckets_[api_key_header].consume()) {
                    json_error(res, 429, "Rate limit exceeded: 100 requests per minute");
                    return;
                }
            }
            if (!tenant_mgr_->route_ingest(api_key_header, app, level, message, ts_ns)) {
                json_error(res, 401, "Unauthorized: unknown project API key");
                return;
            }
        } else {
            // Legacy single-tenant path
            if (!authenticate(req, res)) return;
            if (insert_cb_) insert_cb_(app, level, message, ts_ns);
        }

        // Push to ring buffer for pipeline processing (both paths)
        LogEntry entry;
        entry.line         = message;
        entry.source       = "http:" + app;
        entry.service_name = app;
        entry.timestamp    = std::chrono::steady_clock::now();
        entry.line_number  = 0;
        ring_buffer_.try_push(entry);

        total_ingested_.fetch_add(1, std::memory_order_relaxed);
        print_log(app, level, message);

        nlohmann::json resp;
        resp["status"]   = "accepted";
        resp["app"]      = app;
        resp["level"]    = level;
        resp["ingested"] = total_ingested_.load(std::memory_order_relaxed);
        res.set_content(resp.dump(), "application/json");
    }

    // ─── Per-project data routes (JWT Bearer) ─────────────────────────────────

    void handle_proj_query(const httplib::Request& req, httplib::Response& res) {
        JwtClaims claims;
        if (!auth_bearer(req, res, claims)) return;
        std::string pid = req.matches[1].str();
        if (!verify_ownership(pid, claims, res)) return;
        auto* tsdb = tenant_mgr_ ? tenant_mgr_->get_tsdb(pid) : nullptr;
        if (!tsdb) { res.set_content("[]", "application/json"); return; }

        std::string app   = req.has_param("app")   ? req.get_param_value("app")   : "";
        std::string level = req.has_param("level") ? req.get_param_value("level") : "";
        int last_sec = 3600;
        if (req.has_param("last")) try { last_sec = std::stoi(req.get_param_value("last")); } catch (...) {}
        res.set_content(tsdb->query_to_json(app, level, last_sec), "application/json");
    }

    void handle_proj_stats(const httplib::Request& req, httplib::Response& res) {
        JwtClaims claims;
        if (!auth_bearer(req, res, claims)) return;
        std::string pid = req.matches[1].str();
        if (!verify_ownership(pid, claims, res)) return;
        auto* tsdb = tenant_mgr_ ? tenant_mgr_->get_tsdb(pid) : nullptr;
        if (!tsdb) { res.set_content(R"({"counts":{}})", "application/json"); return; }

        std::string app = req.has_param("app") ? req.get_param_value("app") : "";
        int last_sec = 3600;
        if (req.has_param("last")) try { last_sec = std::stoi(req.get_param_value("last")); } catch (...) {}
        res.set_content(tsdb->stats_to_json(app, last_sec), "application/json");
    }

    void handle_proj_apps(const httplib::Request& req, httplib::Response& res) {
        JwtClaims claims;
        if (!auth_bearer(req, res, claims)) return;
        std::string pid = req.matches[1].str();
        if (!verify_ownership(pid, claims, res)) return;
        auto* tsdb = tenant_mgr_ ? tenant_mgr_->get_tsdb(pid) : nullptr;
        if (!tsdb) { res.set_content("[]", "application/json"); return; }
        res.set_content(tsdb->apps_to_json(), "application/json");
    }

    void handle_proj_metrics(const httplib::Request& req, httplib::Response& res) {
        JwtClaims claims;
        if (!auth_bearer(req, res, claims)) return;
        std::string pid = req.matches[1].str();
        if (!verify_ownership(pid, claims, res)) return;
        auto* tsdb = tenant_mgr_ ? tenant_mgr_->get_tsdb(pid) : nullptr;
        if (!tsdb) { res.set_content("# No data\n", "text/plain; version=0.0.4"); return; }
        res.set_content(tsdb->metrics_to_prometheus(), "text/plain; version=0.0.4");
    }

    void handle_proj_rules(const httplib::Request& req, httplib::Response& res) {
        JwtClaims claims;
        if (!auth_bearer(req, res, claims)) return;
        std::string pid = req.matches[1].str();
        if (!verify_ownership(pid, claims, res)) return;
        // For now return the global rules (per-project rules are a future enhancement)
        if (rules_cb_) { res.set_content(rules_cb_(), "application/json"); return; }
        res.set_content(R"({"rules":[]})", "application/json");
    }

    void handle_proj_history(const httplib::Request& req, httplib::Response& res) {
        JwtClaims claims;
        if (!auth_bearer(req, res, claims)) return;
        std::string pid = req.matches[1].str();
        if (!verify_ownership(pid, claims, res)) return;
        if (history_cb_) { res.set_content(history_cb_(), "application/json"); return; }
        res.set_content(R"({"history":[]})", "application/json");
    }

    // ─── Legacy single-tenant endpoints ──────────────────────────────────────

    void handle_query(const httplib::Request& req, httplib::Response& res) {
        if (!authenticate(req, res)) return;
        if (!query_cb_) { json_error(res, 503, "Storage not available"); return; }
        std::string app   = req.has_param("app")   ? req.get_param_value("app")   : "";
        std::string level = req.has_param("level") ? req.get_param_value("level") : "";
        int last_sec = 300;
        if (req.has_param("last")) try { last_sec = std::stoi(req.get_param_value("last")); } catch (...) {}
        res.set_content(query_cb_(app, level, last_sec), "application/json");
    }

    void handle_stats(const httplib::Request& req, httplib::Response& res) {
        if (!authenticate(req, res)) return;
        if (!stats_cb_) { json_error(res, 503, "Storage not available"); return; }
        std::string app = req.has_param("app") ? req.get_param_value("app") : "";
        int last_sec = 3600;
        if (req.has_param("last")) try { last_sec = std::stoi(req.get_param_value("last")); } catch (...) {}
        res.set_content(stats_cb_(app, last_sec), "application/json");
    }

    void handle_apps(const httplib::Request& req, httplib::Response& res) {
        if (!authenticate(req, res)) return;
        if (!apps_cb_) { res.set_content("[]", "application/json"); return; }
        res.set_content(apps_cb_(), "application/json");
    }

    void handle_metrics(const httplib::Request& req, httplib::Response& res) {
        if (!authenticate(req, res)) return;
        if (!metrics_cb_) { res.set_content("# No data\n", "text/plain; version=0.0.4"); return; }
        res.set_content(metrics_cb_(), "text/plain; version=0.0.4");
    }

    void handle_rules(const httplib::Request& req, httplib::Response& res) {
        if (!authenticate(req, res)) return;
        if (!rules_cb_) { res.set_content(R"({"rules":[]})", "application/json"); return; }
        res.set_content(rules_cb_(), "application/json");
    }

    void handle_history(const httplib::Request& req, httplib::Response& res) {
        if (!authenticate(req, res)) return;
        if (!history_cb_) { res.set_content(R"({"history":[]})", "application/json"); return; }
        res.set_content(history_cb_(), "application/json");
    }

    // ─── Utilities ───────────────────────────────────────────────────────────

    static void json_error(httplib::Response& res, int status, const std::string& msg) {
        res.status = status;
        nlohmann::json j = {{"error", msg}};
        res.set_content(j.dump(), "application/json");
    }

    void print_log(const std::string& app, const std::string& level,
                   const std::string& message) const {
        const char* color = ansi::GREEN;
        if (level == "ERROR")         color = ansi::RED;
        else if (level == "WARN")     color = ansi::YELLOW;
        else if (level == "CRITICAL") color = ansi::BOLD_RED;

        auto now_t = std::chrono::system_clock::to_time_t(
            std::chrono::system_clock::now());
        char time_buf[20];
        std::strftime(time_buf, sizeof(time_buf), "%H:%M:%S",
                      std::localtime(&now_t));

        std::cout << color
                  << "[" << time_buf << "] "
                  << "[" << level << "] "
                  << app << ": " << message
                  << ansi::RESET << "\n";
    }

    // ─── Members ─────────────────────────────────────────────────────────────
    LogRingBuffer&     ring_buffer_;
    uint16_t           port_;
    std::string        api_key_;
    std::atomic<bool>& stop_flag_;

    // SaaS components (null = legacy single-tenant mode)
    TenantManager*  tenant_mgr_    = nullptr;
    AuthManager*    auth_mgr_      = nullptr;
    UserStore*      user_store_    = nullptr;
    ProjectStore*   project_store_ = nullptr;

    httplib::Server  svr_;
    std::thread      server_thread_;
    std::atomic<uint64_t> total_ingested_{0};

    // Legacy callbacks
    InsertCb  insert_cb_;
    QueryCb   query_cb_;
    StatsCb   stats_cb_;
    RulesCb   rules_cb_;
    HistoryCb history_cb_;
    AppsCb    apps_cb_;
    MetricsCb metrics_cb_;

    // Rate limiting (shared by both paths)
    mutable std::shared_mutex rate_mutex_;
    std::unordered_map<std::string, TokenBucket> rate_buckets_;
};

} // namespace logmonitor
