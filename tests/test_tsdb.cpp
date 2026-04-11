/**
 * @file test_tsdb.cpp
 * @brief Tests for the Phase 2 Time-Series Database (TSDB).
 *
 * Tests:
 *  1. Insert + getLogs round-trip (by app and level)
 *  2. getCount filters by level
 *  3. getCount filters by time window (old entries excluded)
 *  4. getApps returns all inserted app names
 *  5. getLastSeen returns max timestamp
 *  6. getCountsByLevel returns per-level breakdown
 *  7. Binary persistence: write + reload
 *  8. Out-of-order insert stays sorted
 *  9. Empty app query returns empty result
 */

#include "TSDB.hpp"

#include <atomic>
#include <cassert>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

static int g_tests_run    = 0;
static int g_tests_passed = 0;

#define ASSERT_EQ(a, b)                                                   \
    do {                                                                   \
        ++g_tests_run;                                                     \
        if ((a) != (b)) {                                                  \
            std::cerr << "FAIL [" << __FILE__ << ":" << __LINE__ << "] "  \
                      << #a << " == " << #b << "\n"                        \
                      << "  got: " << (a) << "\n";                        \
        } else {                                                           \
            ++g_tests_passed;                                              \
        }                                                                  \
    } while (false)

#define ASSERT_TRUE(cond)                                                  \
    do {                                                                   \
        ++g_tests_run;                                                     \
        if (!(cond)) {                                                     \
            std::cerr << "FAIL [" << __FILE__ << ":" << __LINE__ << "] "  \
                      << #cond << " was false\n";                          \
        } else {                                                           \
            ++g_tests_passed;                                              \
        }                                                                  \
    } while (false)

static int64_t now_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

// ─── Tests ───

void test_insert_and_query() {
    std::filesystem::remove("/tmp/tsdb_test_1.bin");
    std::atomic<bool> stop{false};
    logmonitor::TSDB db("/tmp/tsdb_test_1.bin", stop);

    auto ts = now_ns();
    db.insert({"myapp", "ERROR", "disk full",  ts});
    db.insert({"myapp", "WARN",  "high memory", ts + 1});
    db.insert({"myapp", "ERROR", "null ptr",   ts + 2});

    auto errors = db.getLogs("myapp", "ERROR", 3600);
    ASSERT_EQ(static_cast<int>(errors.size()), 2);

    auto warns = db.getLogs("myapp", "WARN", 3600);
    ASSERT_EQ(static_cast<int>(warns.size()), 1);
    ASSERT_EQ(warns[0].message, std::string("high memory"));

    auto all = db.getLogs("myapp", "", 3600);
    ASSERT_EQ(static_cast<int>(all.size()), 3);
}

void test_get_count() {
    std::filesystem::remove("/tmp/tsdb_test_2.bin");
    std::atomic<bool> stop{false};
    logmonitor::TSDB db("/tmp/tsdb_test_2.bin", stop);

    auto ts = now_ns();
    for (int i = 0; i < 5; ++i)
        db.insert({"api", "ERROR", "failed", ts + i});
    for (int i = 0; i < 3; ++i)
        db.insert({"api", "INFO",  "ok",     ts + 10 + i});

    ASSERT_EQ(db.getCount("api", "ERROR", 3600), 5);
    ASSERT_EQ(db.getCount("api", "INFO",  3600), 3);
    ASSERT_EQ(db.getCount("api", "",      3600), 8);
    ASSERT_EQ(db.getCount("api", "WARN",  3600), 0);
}

void test_time_window_filter() {
    std::filesystem::remove("/tmp/tsdb_test_3.bin");
    std::atomic<bool> stop{false};
    logmonitor::TSDB db("/tmp/tsdb_test_3.bin", stop);

    // Insert old entry (2 hours ago)
    int64_t two_hours_ago = now_ns() - 2LL * 3600 * 1'000'000'000LL;
    db.insert({"svc", "ERROR", "old error", two_hours_ago});

    // Insert recent entry (now)
    db.insert({"svc", "ERROR", "new error", now_ns()});

    // 5-minute window: only new entry
    ASSERT_EQ(db.getCount("svc", "ERROR", 300), 1);

    // 3-hour window: both entries
    ASSERT_EQ(db.getCount("svc", "ERROR", 10800), 2);
}

void test_get_apps() {
    std::filesystem::remove("/tmp/tsdb_test_4.bin");
    std::atomic<bool> stop{false};
    logmonitor::TSDB db("/tmp/tsdb_test_4.bin", stop);

    db.insert({"alpha", "INFO",  "start", now_ns()});
    db.insert({"beta",  "ERROR", "crash", now_ns()});
    db.insert({"gamma", "WARN",  "slow",  now_ns()});

    auto apps = db.getApps();
    ASSERT_EQ(static_cast<int>(apps.size()), 3);

    // Check all three are present
    bool found_alpha = false, found_beta = false, found_gamma = false;
    for (const auto& a : apps) {
        if (a == "alpha") found_alpha = true;
        if (a == "beta")  found_beta  = true;
        if (a == "gamma") found_gamma = true;
    }
    ASSERT_TRUE(found_alpha);
    ASSERT_TRUE(found_beta);
    ASSERT_TRUE(found_gamma);
}

void test_last_seen() {
    std::filesystem::remove("/tmp/tsdb_test_5.bin");
    std::atomic<bool> stop{false};
    logmonitor::TSDB db("/tmp/tsdb_test_5.bin", stop);

    int64_t ts1 = now_ns();
    int64_t ts2 = ts1 + 1'000'000'000LL;  // 1 second later
    db.insert({"tracker", "INFO", "first",  ts1});
    db.insert({"tracker", "INFO", "second", ts2});

    ASSERT_EQ(db.getLastSeen("tracker"), ts2);
    ASSERT_EQ(db.getLastSeen("nonexistent"), static_cast<int64_t>(0));
}

void test_counts_by_level() {
    std::filesystem::remove("/tmp/tsdb_test_6.bin");
    std::atomic<bool> stop{false};
    logmonitor::TSDB db("/tmp/tsdb_test_6.bin", stop);

    auto ts = now_ns();
    db.insert({"app", "ERROR",    "e1", ts});
    db.insert({"app", "ERROR",    "e2", ts + 1});
    db.insert({"app", "WARN",     "w1", ts + 2});
    db.insert({"app", "CRITICAL", "c1", ts + 3});

    auto counts = db.getCountsByLevel("app", 3600);
    ASSERT_EQ(counts.at("ERROR"),    2);
    ASSERT_EQ(counts.at("WARN"),     1);
    ASSERT_EQ(counts.at("CRITICAL"), 1);
    ASSERT_EQ(counts.at("INFO"),     0);
}

void test_persistence_roundtrip() {
    const std::string path = "/tmp/tsdb_persist_test.bin";
    std::filesystem::remove(path);
    std::filesystem::remove(path + ".tmp");

    int64_t ts = now_ns();

    // Write phase
    {
        std::atomic<bool> stop{false};
        logmonitor::TSDB db(path, stop);
        db.insert({"persist_app", "ERROR", "persisted error", ts});
        db.insert({"persist_app", "INFO",  "persisted info",  ts + 1});
        // Manually trigger write via stop (calls write_to_disk in destructor)
        stop.store(true);
        db.stop();
    }

    // Read phase: a new TSDB instance should load from disk
    {
        std::atomic<bool> stop2{false};
        logmonitor::TSDB db2(path, stop2);

        auto entries = db2.getLogs("persist_app", "", 3600);
        ASSERT_EQ(static_cast<int>(entries.size()), 2);
        ASSERT_EQ(db2.getCount("persist_app", "ERROR", 3600), 1);

        stop2.store(true);
        db2.stop();
    }

    std::filesystem::remove(path);
}

void test_empty_app_query() {
    std::filesystem::remove("/tmp/tsdb_test_empty.bin");
    std::atomic<bool> stop{false};
    logmonitor::TSDB db("/tmp/tsdb_test_empty.bin", stop);

    auto result = db.getLogs("nonexistent_app", "ERROR", 3600);
    ASSERT_EQ(static_cast<int>(result.size()), 0);
    ASSERT_EQ(db.getCount("nonexistent_app", "ERROR", 3600), 0);
}

void test_json_output() {
    std::filesystem::remove("/tmp/tsdb_test_json.bin");
    std::atomic<bool> stop{false};
    logmonitor::TSDB db("/tmp/tsdb_test_json.bin", stop);

    db.insert({"jsonapp", "ERROR", "test error", now_ns()});

    auto json_str  = db.query_to_json("jsonapp", "ERROR", 3600);
    auto stats_str = db.stats_to_json("jsonapp", 3600);
    auto apps_str  = db.apps_to_json();
    auto prom_str  = db.metrics_to_prometheus();

    ASSERT_TRUE(json_str.find("test error") != std::string::npos);
    ASSERT_TRUE(stats_str.find("jsonapp")   != std::string::npos);
    ASSERT_TRUE(apps_str.find("jsonapp")    != std::string::npos);
    ASSERT_TRUE(prom_str.find("log_count")  != std::string::npos);
}

int main() {
    std::cout << "=== TSDB Tests ===\n\n";

    test_insert_and_query();
    test_get_count();
    test_time_window_filter();
    test_get_apps();
    test_last_seen();
    test_counts_by_level();
    test_persistence_roundtrip();
    test_empty_app_query();
    test_json_output();

    std::cout << "\n" << g_tests_passed << "/" << g_tests_run << " tests passed\n";

    if (g_tests_passed != g_tests_run) {
        std::cerr << (g_tests_run - g_tests_passed) << " test(s) FAILED\n";
        return 1;
    }
    std::cout << "ALL TESTS PASSED\n";
    return 0;
}
