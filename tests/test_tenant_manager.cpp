/**
 * @file test_tenant_manager.cpp
 * @brief Unit tests for TenantManager.
 *
 * Tests (5):
 *  1. route_ingest with valid API key succeeds and data is in TSDB
 *  2. route_ingest with unknown API key returns false
 *  3. Lazy TSDB creation: get_tsdb creates on first call, reuses after
 *  4. Two projects produce isolated TSDbs (no cross-contamination)
 *  5. default project routing via ingest_to_project
 */

#include "TenantManager.hpp"
#include "ProjectStore.hpp"
#include "AuthManager.hpp"

#include <atomic>
#include <cassert>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <string>
#include <thread>

using namespace logmonitor;
namespace fs = std::filesystem;

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

static const std::string PROJ_PATH = "/tmp/test_tm_projects.json";
static const std::string DATA_DIR  = "/tmp/test_tm_data";

static void cleanup() {
    fs::remove(PROJ_PATH);
    fs::remove(PROJ_PATH + ".tmp");
    fs::remove_all(DATA_DIR);
}

static int64_t now_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

int main() {
    std::cout << "=== TenantManager Tests ===\n";
    cleanup();

    // 1. route_ingest with valid key
    TEST("route_ingest with valid API key inserts entry into project TSDB") {
        cleanup();
        std::atomic<bool> stop{false};
        ProjectStore ps(PROJ_PATH);
        Project proj;
        ps.create_project("App1", "owner1", proj);

        TenantManager tm(ps, DATA_DIR, stop);

        bool ok = tm.route_ingest(proj.api_key, "myapp", "ERROR",
                                  "disk full", now_ns());
        assert(ok);

        // Allow async TSDB insert to settle
        std::this_thread::sleep_for(std::chrono::milliseconds(50));

        TSDB* tsdb = tm.get_tsdb(proj.id);
        assert(tsdb != nullptr);

        // Count should be at least 1
        int count = tsdb->getCount("myapp", "ERROR", 300);
        assert(count >= 1);

        stop.store(true, std::memory_order_relaxed);
        tm.stop_all();
    } PASS

    // 2. unknown API key
    TEST("route_ingest with unknown API key returns false") {
        cleanup();
        std::atomic<bool> stop{false};
        ProjectStore ps(PROJ_PATH);

        TenantManager tm(ps, DATA_DIR, stop);
        bool ok = tm.route_ingest("lm_proj_nonexistent00000000000000000", "app",
                                  "INFO", "msg", now_ns());
        assert(!ok);
        stop.store(true, std::memory_order_relaxed);
        tm.stop_all();
    } PASS

    // 3. lazy TSDB creation
    TEST("get_tsdb creates TSDB lazily and reuses on second call") {
        cleanup();
        std::atomic<bool> stop{false};
        ProjectStore ps(PROJ_PATH);
        Project proj;
        ps.create_project("Lazy", "owner2", proj);

        TenantManager tm(ps, DATA_DIR, stop);

        TSDB* t1 = tm.get_tsdb(proj.id);
        assert(t1 != nullptr);

        TSDB* t2 = tm.get_tsdb(proj.id);
        assert(t2 == t1);  // same instance

        auto ids = tm.active_project_ids();
        assert(ids.size() == 1);
        assert(ids[0] == proj.id);

        stop.store(true, std::memory_order_relaxed);
        tm.stop_all();
    } PASS

    // 4. two projects isolated
    TEST("Two projects produce isolated TSDbs with no cross-contamination") {
        cleanup();
        std::atomic<bool> stop{false};
        ProjectStore ps(PROJ_PATH);
        Project p1, p2;
        ps.create_project("Alpha", "owner3", p1);
        ps.create_project("Beta",  "owner3", p2);

        TenantManager tm(ps, DATA_DIR, stop);

        tm.ingest_to_project(p1.id, "alpha-svc", "ERROR", "alpha error", now_ns());
        tm.ingest_to_project(p2.id, "beta-svc",  "WARN",  "beta warn",  now_ns());

        std::this_thread::sleep_for(std::chrono::milliseconds(50));

        TSDB* tsdb1 = tm.get_tsdb(p1.id);
        TSDB* tsdb2 = tm.get_tsdb(p2.id);
        assert(tsdb1 != nullptr);
        assert(tsdb2 != nullptr);
        assert(tsdb1 != tsdb2);

        // alpha-svc only in tsdb1
        assert(tsdb1->getCount("alpha-svc", "ERROR", 300) >= 1);
        assert(tsdb2->getCount("alpha-svc", "ERROR", 300) == 0);

        // beta-svc only in tsdb2
        assert(tsdb2->getCount("beta-svc", "WARN", 300) >= 1);
        assert(tsdb1->getCount("beta-svc", "WARN", 300) == 0);

        stop.store(true, std::memory_order_relaxed);
        tm.stop_all();
    } PASS

    // 5. default project routing
    TEST("ingest_to_project routes to default project TSDB") {
        cleanup();
        std::atomic<bool> stop{false};
        ProjectStore ps(PROJ_PATH);
        Project proj;
        ps.create_project("Default", "owner4", proj);

        TenantManager tm(ps, DATA_DIR, stop, proj.id);

        TSDB* def = tm.get_default_tsdb();
        assert(def != nullptr);
        assert(tm.default_project_id() == proj.id);

        tm.ingest_to_project(proj.id, "default-svc", "INFO", "started", now_ns());
        std::this_thread::sleep_for(std::chrono::milliseconds(50));

        assert(def->getCount("default-svc", "INFO", 300) >= 1);
        stop.store(true, std::memory_order_relaxed);
        tm.stop_all();
    } PASS

    cleanup();
    std::cout << "\n  Passed: " << passed << "  Failed: " << failed << "\n";
    return failed == 0 ? 0 : 1;
}
