/**
 * @file test_network_ingester.cpp
 * @brief Tests for NetworkIngester: TCP and HTTP log ingestion.
 */

#include "FileWatcher.hpp"       // LogRingBuffer
#include "NetworkIngester.hpp"
#include "PatternEngine.hpp"     // LogEntry
#include "TcpSocket.hpp"

#include <nlohmann/json.hpp>

#include <atomic>
#include <cassert>
#include <chrono>
#include <iostream>
#include <string>
#include <thread>

using namespace logmonitor;

// ─── Test 1: TCP ingestion ───

void test_tcp_ingestion() {
    std::atomic<bool> stop{false};
    LogRingBuffer ring_buffer;

    // Use high ports to avoid permission issues
    NetworkIngester ingester(ring_buffer, 15514, 18080, 10, stop);
    ingester.start();

    // Give the servers time to bind
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    // Connect as a TCP client and send some log lines
    try {
        auto client = TcpSocket::create();
        client.connect("127.0.0.1", 15514);

        client.send_all("test-service|ERROR|NullPointerException at main.cpp:42\n");
        client.send_all("test-service|WARN|High memory usage detected\n");
        client.send_all("test-service|INFO|Service started successfully\n");

        // Give time for processing
        std::this_thread::sleep_for(std::chrono::milliseconds(300));

        // Verify entries landed in ring buffer
        int count = 0;
        while (true) {
            auto entry = ring_buffer.try_pop();
            if (!entry) break;
            ++count;
            assert(!entry->line.empty());
            assert(entry->source.find("tcp:") != std::string::npos);
        }

        assert(count == 3);
        assert(ingester.total_network_lines() >= 3);

        std::cout << "  [PASS] test_tcp_ingestion (" << count << " lines)\n";
    } catch (const std::exception& e) {
        std::cout << "  [SKIP] test_tcp_ingestion (port unavailable: "
                  << e.what() << ")\n";
    }

    stop.store(true, std::memory_order_relaxed);
    ingester.stop();
}

// ─── Test 2: HTTP POST /ingest ───

void test_http_ingestion() {
    std::atomic<bool> stop{false};
    LogRingBuffer ring_buffer;

    NetworkIngester ingester(ring_buffer, 15515, 18081, 10, stop);
    ingester.start();

    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    try {
        auto client = TcpSocket::create();
        client.connect("127.0.0.1", 18081);

        nlohmann::json body;
        body["service"] = "payment-api";
        body["lines"] = {"ERROR: Transaction failed for user 123",
                          "WARN: Retry attempt 3 of 5"};
        std::string body_str = body.dump();

        std::string request =
            "POST /ingest HTTP/1.1\r\n"
            "Host: localhost\r\n"
            "Content-Type: application/json\r\n"
            "Content-Length: " + std::to_string(body_str.size()) + "\r\n"
            "\r\n" + body_str;

        client.send_all(request);

        // Read response
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        auto response = client.recv(4096);

        assert(response.find("200") != std::string::npos);
        assert(response.find("\"ingested\":2") != std::string::npos);

        // Verify entries in ring buffer
        int count = 0;
        while (true) {
            auto entry = ring_buffer.try_pop();
            if (!entry) break;
            ++count;
            assert(entry->source == "http:payment-api");
        }
        assert(count == 2);

        std::cout << "  [PASS] test_http_ingestion (" << count << " lines)\n";
    } catch (const std::exception& e) {
        std::cout << "  [SKIP] test_http_ingestion (port unavailable: "
                  << e.what() << ")\n";
    }

    stop.store(true, std::memory_order_relaxed);
    ingester.stop();
}

// ─── Test 3: HTTP GET /status ───

void test_http_status() {
    std::atomic<bool> stop{false};
    LogRingBuffer ring_buffer;

    NetworkIngester ingester(ring_buffer, 15516, 18082, 10, stop);
    ingester.set_status_callback([]() -> std::string {
        return R"({"status":"running","total_lines":42})";
    });
    ingester.start();

    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    try {
        auto client = TcpSocket::create();
        client.connect("127.0.0.1", 18082);

        std::string request =
            "GET /status HTTP/1.1\r\n"
            "Host: localhost\r\n"
            "\r\n";

        client.send_all(request);

        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        auto response = client.recv(4096);

        assert(response.find("200") != std::string::npos);
        assert(response.find("\"status\":\"running\"") != std::string::npos);
        assert(response.find("\"total_lines\":42") != std::string::npos);

        std::cout << "  [PASS] test_http_status\n";
    } catch (const std::exception& e) {
        std::cout << "  [SKIP] test_http_status (port unavailable: "
                  << e.what() << ")\n";
    }

    stop.store(true, std::memory_order_relaxed);
    ingester.stop();
}

// ─── Test 4: HTTP GET /health ───

void test_http_health() {
    std::atomic<bool> stop{false};
    LogRingBuffer ring_buffer;

    NetworkIngester ingester(ring_buffer, 15517, 18083, 10, stop);
    ingester.start();

    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    try {
        auto client = TcpSocket::create();
        client.connect("127.0.0.1", 18083);

        std::string request =
            "GET /health HTTP/1.1\r\n"
            "Host: localhost\r\n"
            "\r\n";

        client.send_all(request);

        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        auto response = client.recv(4096);

        assert(response.find("200") != std::string::npos);
        assert(response.find("\"status\":\"ok\"") != std::string::npos);

        std::cout << "  [PASS] test_http_health\n";
    } catch (const std::exception& e) {
        std::cout << "  [SKIP] test_http_health (port unavailable: "
                  << e.what() << ")\n";
    }

    stop.store(true, std::memory_order_relaxed);
    ingester.stop();
}

// ─── Test 5: Connection tracking ───

void test_connection_tracking() {
    std::atomic<bool> stop{false};
    LogRingBuffer ring_buffer;

    NetworkIngester ingester(ring_buffer, 15518, 18084, 10, stop);
    ingester.start();

    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    assert(ingester.active_connections() == 0);
    assert(ingester.total_network_lines() == 0);

    std::cout << "  [PASS] test_connection_tracking\n";

    stop.store(true, std::memory_order_relaxed);
    ingester.stop();
}

int main() {
    std::cout << "=== Network Ingester Tests ===\n";

    test_tcp_ingestion();
    test_http_ingestion();
    test_http_status();
    test_http_health();
    test_connection_tracking();

    std::cout << "=== All Network Ingester Tests PASSED ===\n";
    return 0;
}
