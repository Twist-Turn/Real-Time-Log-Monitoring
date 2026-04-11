#pragma once
/**
 * @file AlertManager.hpp
 * @brief Handles alert output: colored terminal messages, alerts.log file,
 *        and RAG code location lookups.
 *
 * Thread-safe: file writes are mutex-protected (low frequency). Atomic
 * counters per severity level for dashboard consumption.
 */

#include "PatternEngine.hpp"
#include "CodeIndexer.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>

namespace logmonitor {

class AlertManager {
public:
    explicit AlertManager(const std::string& alert_log_path = "alerts.log",
                          CodeIndexer* code_indexer = nullptr)
        : code_indexer_(code_indexer)
    {
        alert_file_.open(alert_log_path, std::ios::app);
        if (!alert_file_.is_open()) {
            std::cerr << "[WARN] Cannot open " << alert_log_path
                      << " for alert logging\n";
        }
    }

    ~AlertManager() {
        if (alert_file_.is_open()) {
            alert_file_.close();
        }
    }

    // Non-copyable
    AlertManager(const AlertManager&) = delete;
    AlertManager& operator=(const AlertManager&) = delete;

    /**
     * @brief Handle a matched alert. Called from ThreadPool workers.
     *
     * 1. Increment atomic counters
     * 2. Format colored terminal output
     * 3. Write to alerts.log
     * 4. Run RAG query for ERROR/CRITICAL
     */
    void handle_alert(const LogEntry& entry, AlertRule& rule) {
        // 1. Increment counters
        rule.trigger_count.fetch_add(1, std::memory_order_relaxed);
        increment_severity_counter(rule.severity);

        // 2. Get timestamp string
        auto now = std::chrono::system_clock::now();
        auto time_t = std::chrono::system_clock::to_time_t(now);
        std::ostringstream ts;
        ts << std::put_time(std::localtime(&time_t), "%Y-%m-%d %H:%M:%S");

        // 3. Build alert message
        std::ostringstream msg;
        msg << "[" << ts.str() << "] "
            << severity_to_string(rule.severity)
            << " [" << rule.name << "] "
            << "source=" << entry.source;
        if (!entry.service_name.empty()) {
            msg << " service=" << entry.service_name;
        }
        msg << " | " << entry.line;

        // 4. RAG lookup for ERROR/CRITICAL
        std::vector<CodeLocation> code_locs;
        if (code_indexer_ && code_indexer_->is_built() &&
            (rule.severity == Severity::ERROR || rule.severity == Severity::CRITICAL)) {
            code_locs = code_indexer_->query(entry.line);
        }

        // 5. Write to alerts.log (mutex-protected)
        {
            std::lock_guard<std::mutex> lock(file_mutex_);
            if (alert_file_.is_open()) {
                alert_file_ << msg.str() << "\n";
                for (const auto& loc : code_locs) {
                    alert_file_ << "  -> Likely code: "
                                << loc.file_path << ":" << loc.line_number
                                << " in " << loc.function_name
                                << " (score: " << loc.relevance_score << ")\n";
                    if (!loc.snippet.empty()) {
                        alert_file_ << "     " << loc.snippet << "\n";
                    }
                }
                alert_file_.flush();
            }
        }

        // 6. Store last code locations for dashboard
        if (!code_locs.empty()) {
            std::lock_guard<std::mutex> lock(rag_mutex_);
            last_code_locations_ = std::move(code_locs);
        }
    }

    // ─── Atomic severity counters (read by Dashboard) ───

    [[nodiscard]] uint64_t info_count() const {
        return info_count_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] uint64_t warn_count() const {
        return warn_count_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] uint64_t error_count() const {
        return error_count_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] uint64_t critical_count() const {
        return critical_count_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] uint64_t total_alerts() const {
        return info_count() + warn_count() + error_count() + critical_count();
    }

    /// Get the most recent RAG code locations (for dashboard/API)
    [[nodiscard]] std::vector<CodeLocation> get_last_code_locations() const {
        std::lock_guard<std::mutex> lock(rag_mutex_);
        return last_code_locations_;
    }

    void set_code_indexer(CodeIndexer* indexer) {
        code_indexer_ = indexer;
    }

private:
    void increment_severity_counter(Severity sev) {
        switch (sev) {
            case Severity::INFO:     info_count_.fetch_add(1, std::memory_order_relaxed); break;
            case Severity::WARN:     warn_count_.fetch_add(1, std::memory_order_relaxed); break;
            case Severity::ERROR:    error_count_.fetch_add(1, std::memory_order_relaxed); break;
            case Severity::CRITICAL: critical_count_.fetch_add(1, std::memory_order_relaxed); break;
        }
    }

    // Alert counters (lock-free, read by dashboard)
    std::atomic<uint64_t> info_count_{0};
    std::atomic<uint64_t> warn_count_{0};
    std::atomic<uint64_t> error_count_{0};
    std::atomic<uint64_t> critical_count_{0};

    // Alert log file
    std::ofstream alert_file_;
    std::mutex file_mutex_;

    // RAG
    CodeIndexer* code_indexer_;  // Non-owning pointer
    mutable std::mutex rag_mutex_;
    std::vector<CodeLocation> last_code_locations_;
};

} // namespace logmonitor
