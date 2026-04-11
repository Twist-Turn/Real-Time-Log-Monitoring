#pragma once
/**
 * @file StatsAggregator.hpp
 * @brief Periodic stats dumper — writes JSON snapshots to stats.json.
 *
 * Runs in a dedicated thread, waking every N seconds to read atomic
 * counters and compute deltas for throughput metrics.
 */

#include "AlertManager.hpp"
#include "LogPipeline.hpp"
#include "NetworkIngester.hpp"
#include "PatternEngine.hpp"

#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <string>
#include <thread>

namespace logmonitor {

class StatsAggregator {
public:
    StatsAggregator(const LogPipeline& pipeline,
                    const AlertManager& alert_mgr,
                    const PatternEngine& engine,
                    const NetworkIngester* network,
                    std::atomic<bool>& stop_flag,
                    std::string output_path = "stats.json",
                    int interval_seconds = 5)
        : pipeline_(pipeline)
        , alert_mgr_(alert_mgr)
        , engine_(engine)
        , network_(network)
        , stop_flag_(stop_flag)
        , output_path_(std::move(output_path))
        , interval_seconds_(interval_seconds)
    {}

    /// Run the stats loop in the current thread (blocking)
    void run() {
        uint64_t prev_lines = 0;
        auto prev_time = std::chrono::steady_clock::now();

        while (!stop_flag_.load(std::memory_order_relaxed)) {
            // Sleep in small increments for responsive shutdown
            for (int i = 0; i < interval_seconds_ * 10; ++i) {
                if (stop_flag_.load(std::memory_order_relaxed)) return;
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }

            auto now = std::chrono::steady_clock::now();
            uint64_t current_lines = pipeline_.total_lines();

            // Compute lines per second
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                now - prev_time).count();
            double lines_per_sec = 0.0;
            if (elapsed > 0) {
                lines_per_sec = static_cast<double>(current_lines - prev_lines)
                    / (static_cast<double>(elapsed) / 1000.0);
            }

            // Build JSON snapshot
            nlohmann::json stats;

            // Timestamp
            auto sys_now = std::chrono::system_clock::now();
            auto time_t = std::chrono::system_clock::to_time_t(sys_now);
            std::ostringstream ts;
            ts << std::put_time(std::localtime(&time_t), "%Y-%m-%dT%H:%M:%S");
            stats["timestamp"] = ts.str();

            // Throughput
            stats["total_lines_processed"] = current_lines;
            stats["lines_per_second"] = static_cast<int64_t>(lines_per_sec);
            stats["total_matched"] = pipeline_.total_matched();
            stats["avg_processing_latency_us"] = pipeline_.avg_latency_us();

            // Alert counts per severity
            stats["alerts"]["info"] = alert_mgr_.info_count();
            stats["alerts"]["warn"] = alert_mgr_.warn_count();
            stats["alerts"]["error"] = alert_mgr_.error_count();
            stats["alerts"]["critical"] = alert_mgr_.critical_count();
            stats["alerts"]["total"] = alert_mgr_.total_alerts();

            // Per-rule stats
            nlohmann::json rules_json = nlohmann::json::array();
            for (const auto& rule : engine_.rules()) {
                nlohmann::json rj;
                rj["name"] = rule->name;
                rj["severity"] = severity_to_string(rule->severity);
                rj["pattern"] = rule->pattern_str;
                rj["trigger_count"] = rule->trigger_count.load(std::memory_order_relaxed);
                rules_json.push_back(std::move(rj));
            }
            stats["rules"] = std::move(rules_json);

            // Network ingestion stats
            if (network_) {
                stats["network"]["total_lines_received"] = network_->total_network_lines();
                stats["network"]["active_connections"] = network_->active_connections();

                nlohmann::json conns = nlohmann::json::array();
                for (const auto& [name, lines] : network_->get_connections()) {
                    nlohmann::json c;
                    c["service"] = name;
                    c["lines_received"] = lines;
                    conns.push_back(std::move(c));
                }
                stats["network"]["connections"] = std::move(conns);
            }

            // Write to file
            std::ofstream out(output_path_);
            if (out.is_open()) {
                out << stats.dump(2) << "\n";
                out.close();
            }

            // Save for throughput calculation
            lines_per_second_.store(static_cast<int64_t>(lines_per_sec),
                                     std::memory_order_relaxed);
            prev_lines = current_lines;
            prev_time = now;
        }
    }

    [[nodiscard]] int64_t lines_per_second() const {
        return lines_per_second_.load(std::memory_order_relaxed);
    }

private:
    const LogPipeline& pipeline_;
    const AlertManager& alert_mgr_;
    const PatternEngine& engine_;
    const NetworkIngester* network_;
    std::atomic<bool>& stop_flag_;
    std::string output_path_;
    int interval_seconds_;

    std::atomic<int64_t> lines_per_second_{0};
};

} // namespace logmonitor
