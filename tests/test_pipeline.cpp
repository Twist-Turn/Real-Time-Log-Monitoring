/**
 * @file test_pipeline.cpp
 * @brief Integration test: RingBuffer → LogPipeline → PatternEngine → AlertManager.
 */

#include "AlertManager.hpp"
#include "CodeIndexer.hpp"
#include "FileWatcher.hpp"    // LogRingBuffer
#include "LogPipeline.hpp"
#include "PatternEngine.hpp"
#include "ThreadPool.hpp"

#include <atomic>
#include <cassert>
#include <chrono>
#include <iostream>
#include <string>
#include <thread>

using namespace logmonitor;

// ─── Test 1: End-to-end pipeline flow ───

void test_pipeline_flow() {
    std::atomic<bool> stop{false};
    LogRingBuffer ring_buffer;
    ThreadPool pool(2);
    PatternEngine engine;
    engine.add_rule("errors", "ERROR", Severity::ERROR);
    engine.add_rule("warnings", "WARN", Severity::WARN);

    AlertManager alert_mgr("/tmp/test_pipeline_alerts.log");
    LogPipeline pipeline(ring_buffer, pool, engine, alert_mgr, stop);

    // Start pipeline in background
    std::thread pipeline_thread([&]() { pipeline.run(); });

    // Push some log entries
    for (int i = 0; i < 100; ++i) {
        LogEntry entry;
        if (i % 3 == 0) {
            entry.line = "ERROR: something failed #" + std::to_string(i);
        } else if (i % 3 == 1) {
            entry.line = "WARN: caution #" + std::to_string(i);
        } else {
            entry.line = "DEBUG: normal line #" + std::to_string(i);
        }
        entry.source = "/tmp/test.log";
        entry.timestamp = std::chrono::steady_clock::now();
        entry.line_number = static_cast<uint64_t>(i + 1);

        while (!ring_buffer.try_push(std::move(entry))) {
            std::this_thread::yield();
        }
    }

    // Wait for pipeline to process
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    stop.store(true, std::memory_order_relaxed);
    pipeline_thread.join();
    pool.shutdown();

    assert(pipeline.total_lines() == 100);
    // 34 ERRORs (i=0,3,6,...,99) + 33 WARNs (i=1,4,7,...,97)
    assert(pipeline.total_matched() == 67);
    assert(alert_mgr.error_count() == 34);
    assert(alert_mgr.warn_count() == 33);

    std::cout << "  [PASS] test_pipeline_flow (100 lines, "
              << pipeline.total_matched() << " matched)\n";
}

// ─── Test 2: Recent lines tracking ───

void test_recent_lines() {
    std::atomic<bool> stop{false};
    LogRingBuffer ring_buffer;
    ThreadPool pool(2);
    PatternEngine engine;
    AlertManager alert_mgr("/tmp/test_recent_alerts.log");
    LogPipeline pipeline(ring_buffer, pool, engine, alert_mgr, stop);

    std::thread pipeline_thread([&]() { pipeline.run(); });

    // Push lines from two sources
    for (int i = 0; i < 5; ++i) {
        LogEntry entry;
        entry.line = "Line from source A #" + std::to_string(i);
        entry.source = "source_a";
        entry.timestamp = std::chrono::steady_clock::now();
        while (!ring_buffer.try_push(std::move(entry))) {
            std::this_thread::yield();
        }
    }

    for (int i = 0; i < 3; ++i) {
        LogEntry entry;
        entry.line = "Line from source B #" + std::to_string(i);
        entry.source = "source_b";
        entry.timestamp = std::chrono::steady_clock::now();
        while (!ring_buffer.try_push(std::move(entry))) {
            std::this_thread::yield();
        }
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    auto sources = pipeline.get_sources();
    assert(sources.size() == 2);

    auto lines_a = pipeline.get_recent_lines("source_a");
    assert(lines_a.size() == 5);

    auto lines_b = pipeline.get_recent_lines("source_b");
    assert(lines_b.size() == 3);

    stop.store(true, std::memory_order_relaxed);
    pipeline_thread.join();
    pool.shutdown();

    std::cout << "  [PASS] test_recent_lines\n";
}

// ─── Test 3: Latency tracking ───

void test_latency_tracking() {
    std::atomic<bool> stop{false};
    LogRingBuffer ring_buffer;
    ThreadPool pool(2);
    PatternEngine engine;
    AlertManager alert_mgr("/tmp/test_latency_alerts.log");
    LogPipeline pipeline(ring_buffer, pool, engine, alert_mgr, stop);

    std::thread pipeline_thread([&]() { pipeline.run(); });

    for (int i = 0; i < 10; ++i) {
        LogEntry entry;
        entry.line = "test line " + std::to_string(i);
        entry.source = "test";
        entry.timestamp = std::chrono::steady_clock::now();
        while (!ring_buffer.try_push(std::move(entry))) {
            std::this_thread::yield();
        }
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    // Latency should be positive and reasonable (< 1 second)
    double avg_lat = pipeline.avg_latency_us();
    assert(avg_lat >= 0.0);
    assert(avg_lat < 1000000.0);  // < 1 second

    stop.store(true, std::memory_order_relaxed);
    pipeline_thread.join();
    pool.shutdown();

    std::cout << "  [PASS] test_latency_tracking (avg: "
              << avg_lat << " us)\n";
}

int main() {
    std::cout << "=== Pipeline Integration Tests ===\n";

    test_pipeline_flow();
    test_recent_lines();
    test_latency_tracking();

    std::cout << "=== All Pipeline Tests PASSED ===\n";
    return 0;
}
