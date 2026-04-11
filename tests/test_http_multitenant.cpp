/**
 * @file test_http_multitenant.cpp
 * @brief Integration tests for multi-tenant HttpServer (port 19091).
 *
 * Tests (12):
 *  1.  POST /auth/register — creates user, returns user_id
 *  2.  POST /auth/login    — returns JWT token
 *  3.  POST /auth/login with wrong password — 401
 *  4.  POST /api/projects  — creates project, returns api_key
 *  5.  GET  /api/projects  — lists user's projects
 *  6.  POST /ingest with valid project key — 200
 *  7.  POST /ingest with invalid key — 401
 *  8.  GET  /api/projects/{id}/query — returns logs
 *  9.  GET  /api/projects/{id}/query for project owned by other user — 403
 *  10. POST /api/projects/{id}/rotate-key — new key works, old key fails
 *  11. DELETE /api/projects/{id} — project gone
 *  12. GET  /api/projects unauthenticated — 401
 */

#include "HttpServer.hpp"
#include "AuthManager.hpp"
#include "UserStore.hpp"
#include "ProjectStore.hpp"
#include "TenantManager.hpp"
#include "PatternEngine.hpp"
#include "RingBuffer.hpp"

#include <httplib.h>
#include <nlohmann/json.hpp>

#include <atomic>
#include <cassert>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <thread>

using namespace logmonitor;
using json = nlohmann::json;
namespace fs = std::filesystem;

static constexpr uint16_t PORT = 19091;

static int passed = 0;
static int failed = 0;

#define TEST(name) \
    std::cout << "  " << (name) << " ... "; \
    try {

#define PASS \
    passed++; \
    std::cout << "PASS\n"; \
    } catch (const std::exception& ex) { \
        failed++; \
        std::cout << "FAIL (" << ex.what() << ")\n"; \
    } catch (...) { \
        failed++; \
        std::cout << "FAIL (unknown exception)\n"; \
    }

static const std::string USERS_PATH = "/tmp/test_mt_users.json";
static const std::string PROJ_PATH  = "/tmp/test_mt_projects.json";
static const std::string DATA_DIR   = "/tmp/test_mt_data";

static void cleanup() {
    fs::remove(USERS_PATH);
    fs::remove(PROJ_PATH);
    fs::remove(USERS_PATH + ".tmp");
    fs::remove(PROJ_PATH  + ".tmp");
    fs::remove_all(DATA_DIR);
}

static std::string json_post(httplib::Client& cli,
                              const std::string& path,
                              const json& body,
                              const std::string& token = "") {
    httplib::Headers headers;
    if (!token.empty())
        headers.emplace("Authorization", "Bearer " + token);
    auto res = cli.Post(path, headers, body.dump(), "application/json");
    if (!res) throw std::runtime_error("POST " + path + " failed (no response)");
    return res->body;
}

static std::string json_get(httplib::Client& cli,
                             const std::string& path,
                             const std::string& token = "") {
    httplib::Headers headers;
    if (!token.empty())
        headers.emplace("Authorization", "Bearer " + token);
    auto res = cli.Get(path, headers);
    if (!res) throw std::runtime_error("GET " + path + " failed (no response)");
    return res->body;
}

static int status_post(httplib::Client& cli,
                        const std::string& path,
                        const json& body,
                        const std::string& token = "") {
    httplib::Headers headers;
    if (!token.empty())
        headers.emplace("Authorization", "Bearer " + token);
    auto res = cli.Post(path, headers, body.dump(), "application/json");
    if (!res) return -1;
    return res->status;
}

static int status_get(httplib::Client& cli,
                       const std::string& path,
                       const std::string& token = "") {
    httplib::Headers headers;
    if (!token.empty())
        headers.emplace("Authorization", "Bearer " + token);
    auto res = cli.Get(path, headers);
    if (!res) return -1;
    return res->status;
}

static int status_delete(httplib::Client& cli,
                          const std::string& path,
                          const std::string& token = "") {
    httplib::Headers headers;
    if (!token.empty())
        headers.emplace("Authorization", "Bearer " + token);
    auto res = cli.Delete(path, headers);
    if (!res) return -1;
    return res->status;
}

int main() {
    std::cout << "=== HTTP Multi-Tenant Tests (port " << PORT << ") ===\n";
    cleanup();

    // ─── Setup ───────────────────────────────────────────────────────────────
    std::atomic<bool> stop{false};
    auto ring_buffer   = std::make_unique<LogRingBuffer>();
    auto auth_mgr      = std::make_unique<AuthManager>("multitenant-test-secret!!");
    auto user_store    = std::make_unique<UserStore>(USERS_PATH);
    auto project_store = std::make_unique<ProjectStore>(PROJ_PATH);
    auto tenant_mgr    = std::make_unique<TenantManager>(*project_store, DATA_DIR, stop);

    auto server = std::make_unique<HttpServer>(
        *ring_buffer, PORT, "legacy-key", stop, 4,
        tenant_mgr.get(), auth_mgr.get(), user_store.get(), project_store.get());

    server->start();
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    httplib::Client cli("127.0.0.1", PORT);
    cli.set_connection_timeout(3);
    cli.set_read_timeout(3);

    // Shared state across tests
    std::string alice_token, alice_id;
    std::string bob_token,   bob_id;
    std::string proj1_id, proj1_key;
    std::string proj2_id, proj2_key;

    // 1. register
    TEST("POST /auth/register creates user and returns user_id") {
        auto body = json_post(cli, "/auth/register",
                              {{"email","alice@test.com"},{"password","password123"}});
        auto j = json::parse(body);
        assert(j.contains("user_id"));
        alice_id = j["user_id"].get<std::string>();
        assert(!alice_id.empty());
    } PASS

    // 2. login
    TEST("POST /auth/login returns JWT token") {
        auto body = json_post(cli, "/auth/login",
                              {{"email","alice@test.com"},{"password","password123"}});
        auto j = json::parse(body);
        assert(j.contains("token"));
        alice_token = j["token"].get<std::string>();
        assert(!alice_token.empty());
        assert(j.value("expires_in", 0) > 0);
    } PASS

    // 3. wrong password
    TEST("POST /auth/login with wrong password returns 401") {
        int code = status_post(cli, "/auth/login",
                               {{"email","alice@test.com"},{"password","wrongpassword"}});
        assert(code == 401);
    } PASS

    // 4. create project
    TEST("POST /api/projects creates project and returns api_key") {
        auto body = json_post(cli, "/api/projects",
                              {{"name","Alice Project"}}, alice_token);
        auto j = json::parse(body);
        assert(j.contains("id"));
        assert(j.contains("api_key"));
        proj1_id  = j["id"].get<std::string>();
        proj1_key = j["api_key"].get<std::string>();
        assert(proj1_key.substr(0, 8) == "lm_proj_");
    } PASS

    // 5. list projects
    TEST("GET /api/projects lists user's projects") {
        auto body = json_get(cli, "/api/projects", alice_token);
        auto j = json::parse(body);
        assert(j.is_array());
        assert(j.size() >= 1);
        bool found = false;
        for (auto& p : j) if (p["id"] == proj1_id) found = true;
        assert(found);
    } PASS

    // 6. ingest with valid key
    TEST("POST /ingest with valid project key returns 200") {
        httplib::Headers h;
        h.emplace("X-API-Key", proj1_key);
        auto res = cli.Post("/ingest", h,
            json({{"app","myapp"},{"level","ERROR"},{"message","disk full"},{"timestamp",0}}).dump(),
            "application/json");
        assert(res != nullptr);
        assert(res->status == 200);
    } PASS

    // 7. ingest with invalid key
    TEST("POST /ingest with invalid project key returns 401") {
        httplib::Headers h;
        h.emplace("X-API-Key", "lm_proj_badkey000000000000000000000000");
        auto res = cli.Post("/ingest", h,
            json({{"app","myapp"},{"level","ERROR"},{"message","test"},{"timestamp",0}}).dump(),
            "application/json");
        assert(res != nullptr);
        assert(res->status == 401);
    } PASS

    // 8. query project logs
    TEST("GET /api/projects/{id}/query returns ingested logs") {
        // Give TSDB a moment to process
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        auto body = json_get(cli, "/api/projects/" + proj1_id + "/query?last=300", alice_token);
        auto j = json::parse(body);
        assert(j.is_array());
        // We ingested at least one entry above
        assert(j.size() >= 1);
    } PASS

    // 9. cross-tenant access blocked — register bob and try to access alice's project
    TEST("GET /api/projects/{id}/query for other user's project returns 403") {
        // Register + login as bob
        auto rb = json_post(cli, "/auth/register",
                            {{"email","bob@test.com"},{"password","bobpassword"}});
        auto jb = json::parse(rb);
        bob_id = jb.value("user_id", "");

        auto lb = json_post(cli, "/auth/login",
                            {{"email","bob@test.com"},{"password","bobpassword"}});
        bob_token = json::parse(lb).value("token", "");
        assert(!bob_token.empty());

        // Bob tries to access alice's project
        int code = status_get(cli, "/api/projects/" + proj1_id + "/query?last=60", bob_token);
        assert(code == 403);
    } PASS

    // 10. rotate API key
    TEST("POST /api/projects/{id}/rotate-key changes key; old key fails, new key works") {
        auto body = json_post(cli, "/api/projects/" + proj1_id + "/rotate-key",
                              {}, alice_token);
        auto j = json::parse(body);
        assert(j.contains("api_key"));
        std::string new_key = j["api_key"].get<std::string>();
        assert(new_key != proj1_key);
        assert(new_key.substr(0, 8) == "lm_proj_");

        // Old key should now 401
        httplib::Headers h_old;
        h_old.emplace("X-API-Key", proj1_key);
        auto r_old = cli.Post("/ingest", h_old,
            json({{"app","a"},{"level","INFO"},{"message","old"},{"timestamp",0}}).dump(),
            "application/json");
        assert(r_old != nullptr && r_old->status == 401);

        // New key should 200
        httplib::Headers h_new;
        h_new.emplace("X-API-Key", new_key);
        auto r_new = cli.Post("/ingest", h_new,
            json({{"app","a"},{"level","INFO"},{"message","new"},{"timestamp",0}}).dump(),
            "application/json");
        assert(r_new != nullptr && r_new->status == 200);

        proj1_key = new_key;  // update for subsequent tests
    } PASS

    // 11. delete project
    TEST("DELETE /api/projects/{id} removes project") {
        int code = status_delete(cli, "/api/projects/" + proj1_id, alice_token);
        assert(code == 200);

        // Ingest should now fail (project gone)
        httplib::Headers h;
        h.emplace("X-API-Key", proj1_key);
        auto res = cli.Post("/ingest", h,
            json({{"app","a"},{"level","INFO"},{"message","after-delete"},{"timestamp",0}}).dump(),
            "application/json");
        assert(res != nullptr && res->status == 401);
    } PASS

    // 12. unauthenticated /api/projects → 401
    TEST("GET /api/projects without auth token returns 401") {
        int code = status_get(cli, "/api/projects");
        assert(code == 401);
    } PASS

    // ─── Teardown ────────────────────────────────────────────────────────────
    stop.store(true);
    server->stop();
    tenant_mgr->stop_all();
    cleanup();

    std::cout << "\n  Passed: " << passed << "  Failed: " << failed << "\n";
    return failed == 0 ? 0 : 1;
}
