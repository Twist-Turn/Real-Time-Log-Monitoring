/**
 * @file test_http_server.cpp
 * @brief Tests for the Phase 1 HttpServer (cpp-httplib, port 9090, API key auth).
 *
 * Test plan:
 *  1. GET /health returns {"status":"ok"} without auth
 *  2. POST /ingest with valid API key and valid payload returns 200
 *  3. POST /ingest with missing X-API-Key returns 401
 *  4. POST /ingest with wrong API key returns 401
 *  5. POST /ingest with missing required fields returns 400
 *  6. POST /ingest with invalid JSON returns 400
 *  7. GET /query without auth returns 401
 *  8. GET /apps without auth returns 401
 *  9. Rate limiting: > 100 requests/min returns 429
 */

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
#pragma GCC diagnostic ignored "-Wold-style-cast"
#pragma GCC diagnostic ignored "-Wshadow"
#pragma GCC diagnostic ignored "-Wconversion"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#include <httplib.h>
#pragma GCC diagnostic pop

#include "HttpServer.hpp"
#include "FileWatcher.hpp"  // LogRingBuffer

#include <atomic>
#include <cassert>
#include <chrono>
#include <iostream>
#include <string>
#include <thread>

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

static constexpr uint16_t TEST_PORT = 19090;
static constexpr const char* VALID_KEY = "test-secret-key";

// ─── Test fixture ───

struct Fixture {
    logmonitor::LogRingBuffer ring_buffer;
    std::atomic<bool> stop{false};
    logmonitor::HttpServer server;

    Fixture()
        : server(ring_buffer, TEST_PORT, VALID_KEY, stop, 4)
    {
        server.start();
        // Give server a moment to bind
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    ~Fixture() {
        stop.store(true);
        server.stop();
    }
};

// ─── Tests ───

void test_health_no_auth() {
    Fixture f;
    httplib::Client cli("127.0.0.1", TEST_PORT);
    cli.set_connection_timeout(2, 0);

    auto res = cli.Get("/health");
    ASSERT_TRUE(res != nullptr);
    ASSERT_EQ(res->status, 200);
    ASSERT_TRUE(res->body.find("\"ok\"") != std::string::npos);
}

void test_ingest_valid() {
    Fixture f;
    httplib::Client cli("127.0.0.1", TEST_PORT);
    cli.set_connection_timeout(2, 0);

    httplib::Headers headers{{"X-API-Key", VALID_KEY}};
    std::string body = R"({"app":"testapp","level":"ERROR","message":"disk full","timestamp":0})";

    auto res = cli.Post("/ingest", headers, body, "application/json");
    ASSERT_TRUE(res != nullptr);
    ASSERT_EQ(res->status, 200);
    ASSERT_TRUE(res->body.find("\"accepted\"") != std::string::npos);
    ASSERT_TRUE(res->body.find("\"testapp\"") != std::string::npos);
}

void test_ingest_no_api_key() {
    Fixture f;
    httplib::Client cli("127.0.0.1", TEST_PORT);
    cli.set_connection_timeout(2, 0);

    std::string body = R"({"app":"x","level":"INFO","message":"hi"})";
    auto res = cli.Post("/ingest", body, "application/json");
    ASSERT_TRUE(res != nullptr);
    ASSERT_EQ(res->status, 401);
}

void test_ingest_wrong_api_key() {
    Fixture f;
    httplib::Client cli("127.0.0.1", TEST_PORT);
    cli.set_connection_timeout(2, 0);

    httplib::Headers headers{{"X-API-Key", "wrong-key"}};
    std::string body = R"({"app":"x","level":"INFO","message":"hi"})";
    auto res = cli.Post("/ingest", headers, body, "application/json");
    ASSERT_TRUE(res != nullptr);
    ASSERT_EQ(res->status, 401);
}

void test_ingest_missing_fields() {
    Fixture f;
    httplib::Client cli("127.0.0.1", TEST_PORT);
    cli.set_connection_timeout(2, 0);

    httplib::Headers headers{{"X-API-Key", VALID_KEY}};
    // Missing "message"
    std::string body = R"({"app":"x","level":"INFO"})";
    auto res = cli.Post("/ingest", headers, body, "application/json");
    ASSERT_TRUE(res != nullptr);
    ASSERT_EQ(res->status, 400);
}

void test_ingest_invalid_json() {
    Fixture f;
    httplib::Client cli("127.0.0.1", TEST_PORT);
    cli.set_connection_timeout(2, 0);

    httplib::Headers headers{{"X-API-Key", VALID_KEY}};
    std::string body = "not json at all {{{";
    auto res = cli.Post("/ingest", headers, body, "application/json");
    ASSERT_TRUE(res != nullptr);
    ASSERT_EQ(res->status, 400);
}

void test_query_no_auth() {
    Fixture f;
    httplib::Client cli("127.0.0.1", TEST_PORT);
    cli.set_connection_timeout(2, 0);

    auto res = cli.Get("/query?app=testapp&last=300");
    ASSERT_TRUE(res != nullptr);
    ASSERT_EQ(res->status, 401);
}

void test_apps_no_auth() {
    Fixture f;
    httplib::Client cli("127.0.0.1", TEST_PORT);
    cli.set_connection_timeout(2, 0);

    auto res = cli.Get("/apps");
    ASSERT_TRUE(res != nullptr);
    ASSERT_EQ(res->status, 401);
}

void test_ingest_increments_count() {
    Fixture f;
    httplib::Client cli("127.0.0.1", TEST_PORT);
    cli.set_connection_timeout(2, 0);

    httplib::Headers headers{{"X-API-Key", VALID_KEY}};
    std::string body = R"({"app":"myapp","level":"WARN","message":"low memory"})";

    for (int i = 0; i < 3; ++i) {
        cli.Post("/ingest", headers, body, "application/json");
    }
    ASSERT_EQ(f.server.total_ingested(), static_cast<uint64_t>(3));
}

int main() {
    std::cout << "=== HttpServer Tests ===\n\n";

    test_health_no_auth();
    test_ingest_valid();
    test_ingest_no_api_key();
    test_ingest_wrong_api_key();
    test_ingest_missing_fields();
    test_ingest_invalid_json();
    test_query_no_auth();
    test_apps_no_auth();
    test_ingest_increments_count();

    std::cout << "\n" << g_tests_passed << "/" << g_tests_run << " tests passed\n";

    if (g_tests_passed != g_tests_run) {
        std::cerr << (g_tests_run - g_tests_passed) << " test(s) FAILED\n";
        return 1;
    }
    std::cout << "ALL TESTS PASSED\n";
    return 0;
}
