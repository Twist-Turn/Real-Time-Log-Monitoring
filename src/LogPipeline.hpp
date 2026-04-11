#pragma once
/**
 * @file LogPipeline.hpp
 * @brief Single-consumer pipeline that drains the MPSC ring buffer and
 *        dispatches log entries to the thread pool for processing.
 *
 * Data flow: RingBuffer → LogPipeline (consumer) → ThreadPool →
 *            PatternEngine::match() → AlertManager::handle_alert()
 *
 * Also maintains:
 *   - Rolling buffer of recent lines per source (for Dashboard)
 *   - Atomic counters for throughput metrics
 *   - Latency tracking for stats
 */

#include "AlertManager.hpp"
#include "FileWatcher.hpp"    // LogRingBuffer
#include "PatternEngine.hpp"
#include "ThreadPool.hpp"

#include <atomic>
#include <chrono>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace logmonitor {

/// Rolling window of recent log lines for a single source
struct RecentLines {
    static constexpr std::size_t MAX_LINES = 20;
    std::deque<std::string> lines;

    void add(const std::string& line) {
        lines.push_back(line);
        if (lines.size() > MAX_LINES) {
            lines.pop_front();
        }
    }
};

class LogPipeline {
public:
    LogPipeline(LogRingBuffer& ring_buffer,
                ThreadPool& pool,
                PatternEngine& engine,
                AlertManager& alert_mgr,
                std::atomic<bool>& stop_flag)
        : ring_buffer_(ring_buffer)
        , pool_(pool)
        , engine_(engine)
        , alert_mgr_(alert_mgr)
        , stop_flag_(stop_flag)
    {}

    /// Start the consumer loop in the current thread (blocking)
    void run() {
        while (!stop_flag_.load(std::memory_order_relaxed)) {
            auto entry = ring_buffer_.try_pop();
            if (!entry) {
                // Buffer empty — brief pause to avoid busy-spinning
                std::this_thread::sleep_for(std::chrono::microseconds(100));
                continue;
            }

            auto recv_time = std::chrono::steady_clock::now();

            // Update counters
            total_lines_.fetch_add(1, std::memory_order_relaxed);

            // Store in recent lines buffer (for dashboard)
            {
                std::lock_guard<std::mutex> lock(recent_mutex_);
                recent_lines_[entry->source].add(entry->line);
            }

            // Dispatch to thread pool for pattern matching
            // Capture by value since entry will be moved
            auto captured_entry = std::make_shared<LogEntry>(std::move(*entry));
            pool_.enqueue([this, captured_entry, recv_time]() {
                process_entry(*captured_entry, recv_time);
            });
        }

        // Drain remaining entries on shutdown
        drain();
    }

    // ─── Phase 2: TSDB tap callback ───
    // Called from ThreadPool workers after pattern matching. Receives the log
    // entry and the matched rule (may be nullptr if no rule matched).
    using EntryCallback = std::function<void(const LogEntry&, const AlertRule*)>;

    void set_entry_callback(EntryCallback cb) {
        entry_callback_ = std::move(cb);
    }

    // ─── Stats accessors (thread-safe, read by Dashboard & StatsAggregator) ───

    [[nodiscard]] uint64_t total_lines() const {
        return total_lines_.load(std::memory_order_relaxed);
    }

    [[nodiscard]] uint64_t total_matched() const {
        return total_matched_.load(std::memory_order_relaxed);
    }

    /// Get average processing latency in microseconds
    [[nodiscard]] double avg_latency_us() const {
        uint64_t count = latency_count_.load(std::memory_order_relaxed);
        if (count == 0) return 0.0;
        uint64_t total = latency_total_us_.load(std::memory_order_relaxed);
        return static_cast<double>(total) / static_cast<double>(count);
    }

    /// Get recent lines for a given source (for Dashboard)
    [[nodiscard]] std::deque<std::string> get_recent_lines(const std::string& source) const {
        std::lock_guard<std::mutex> lock(recent_mutex_);
        auto it = recent_lines_.find(source);
        if (it != recent_lines_.end()) {
            return it->second.lines;
        }
        return {};
    }

    /// Get all sources with recent lines
    [[nodiscard]] std::vector<std::string> get_sources() const {
        std::lock_guard<std::mutex> lock(recent_mutex_);
        std::vector<std::string> sources;
        sources.reserve(recent_lines_.size());
        for (const auto& [source, _] : recent_lines_) {
            sources.push_back(source);
        }
        return sources;
    }

private:
    void process_entry(const LogEntry& entry,
                       std::chrono::steady_clock::time_point recv_time) {
        auto* rule = engine_.match(entry.line);
        if (rule) {
            total_matched_.fetch_add(1, std::memory_order_relaxed);
            alert_mgr_.handle_alert(entry, *rule);
        }

        // Phase 2: notify TSDB tap (if registered)
        if (entry_callback_) {
            entry_callback_(entry, rule);
        }

        // Record processing latency
        auto now = std::chrono::steady_clock::now();
        auto latency = std::chrono::duration_cast<std::chrono::microseconds>(
            now - recv_time).count();
        latency_total_us_.fetch_add(static_cast<uint64_t>(latency),
                                     std::memory_order_relaxed);
        latency_count_.fetch_add(1, std::memory_order_relaxed);
    }

    void drain() {
        while (true) {
            auto entry = ring_buffer_.try_pop();
            if (!entry) break;

            total_lines_.fetch_add(1, std::memory_order_relaxed);
            auto* rule = engine_.match(entry->line);
            if (rule) {
                total_matched_.fetch_add(1, std::memory_order_relaxed);
                alert_mgr_.handle_alert(*entry, *rule);
            }
        }
    }

    LogRingBuffer& ring_buffer_;
    ThreadPool& pool_;
    PatternEngine& engine_;
    AlertManager& alert_mgr_;
    std::atomic<bool>& stop_flag_;

    // Phase 2: TSDB tap (optional)
    EntryCallback entry_callback_;

    // Stats
    std::atomic<uint64_t> total_lines_{0};
    std::atomic<uint64_t> total_matched_{0};
    std::atomic<uint64_t> latency_total_us_{0};
    std::atomic<uint64_t> latency_count_{0};

    // Recent lines per source (mutex-protected, read by Dashboard)
    mutable std::mutex recent_mutex_;
    std::unordered_map<std::string, RecentLines> recent_lines_;
};

} // namespace logmonitor
