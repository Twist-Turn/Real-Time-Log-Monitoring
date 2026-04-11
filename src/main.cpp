/**
 * @file main.cpp
 * @brief Orchestrator for the Log Monitoring System (SaaS multi-tenant mode).
 *
 * Parses config, initializes all components, spawns threads, and handles
 * graceful shutdown via SIGINT. Thread lifecycle:
 *
 *   1. FileWatcher threads (one per file)
 *   2. NetworkIngester (TCP + HTTP threads)
 *   3. LogPipeline consumer thread
 *   4. StatsAggregator thread
 *   5. HttpServer (cpp-httplib, port 9090) — serves web dashboard + API
 *
 * Multi-tenant: AuthManager / UserStore / ProjectStore / TenantManager are
 * always initialized; per-project TSDbs are created lazily on first ingest.
 * The legacy single-tenant tsdb is kept for the ncurses Dashboard and the
 * pipeline tap so that file/TCP sources still go somewhere by default.
 *
 * Shutdown: SIGINT → g_stop = true → all threads drain and exit.
 */

#include "AlertManager.hpp"
#include "AlertDelivery.hpp"
#include "AlertRulesEngine.hpp"
#include "AuthManager.hpp"
#include "CodeIndexer.hpp"
#include "Dashboard.hpp"
#include "FileWatcher.hpp"
#include "HttpServer.hpp"
#include "LogPipeline.hpp"
#include "NetworkIngester.hpp"
#include "PatternEngine.hpp"
#include "ProjectStore.hpp"
#include "StatsAggregator.hpp"
#include "TenantManager.hpp"
#include "ThreadPool.hpp"
#include "TSDB.hpp"
#include "UserStore.hpp"

#include <nlohmann/json.hpp>

#include <atomic>
#include <csignal>
#include <filesystem>
#include <unistd.h>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

// ─── Global stop flag (set by SIGINT handler) ───

static std::atomic<bool> g_stop{false};

static void signal_handler(int signum) {
    (void)signum;
    g_stop.store(true, std::memory_order_relaxed);
}

using json = nlohmann::json;

int main(int argc, char* argv[]) {
    // ─── Parse command line ───
    std::string config_path = "config/config.json";
    if (argc > 1) {
        config_path = argv[1];
    }

    // ─── Load config ───
    if (!std::filesystem::exists(config_path)) {
        std::cerr << "Config file not found: " << config_path << "\n";
        std::cerr << "Usage: " << argv[0] << " [config.json]\n";
        return 1;
    }

    json config;
    {
        std::ifstream file(config_path);
        try {
            file >> config;
        } catch (const json::parse_error& e) {
            std::cerr << "Config parse error: " << e.what() << "\n";
            return 1;
        }
    }

    auto config_dir = std::filesystem::path(config_path).parent_path();
    if (config_dir.empty()) config_dir = ".";

    std::cout << "╔══════════════════════════════════════════════╗\n"
              << "║    LogMonitor SaaS v2.0                      ║\n"
              << "║    Open http://localhost:9090 in your browser║\n"
              << "╚══════════════════════════════════════════════╝\n\n";

    // ─── Install signal handler ───
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    // ─── Create shared ring buffer ───
    auto ring_buffer = std::make_unique<logmonitor::LogRingBuffer>();

    // ─── Initialize Pattern Engine ───
    logmonitor::PatternEngine engine;
    logmonitor::load_rules_from_config(engine, config_path);

    // ─── Initialize Code Indexer (RAG) ───
    auto code_indexer = logmonitor::create_code_indexer_from_config(config, config_dir);

    // ─── Initialize Alert Manager ───
    auto alert_mgr = std::make_unique<logmonitor::AlertManager>(
        "alerts.log", code_indexer.get());

    // ─── Initialize Thread Pool ───
    auto pool_size = config.value("thread_pool_size", 4);
    auto thread_pool = std::make_unique<logmonitor::ThreadPool>(
        static_cast<std::size_t>(pool_size));
    std::cout << "[INFO] Thread pool started with " << thread_pool->thread_count()
              << " workers\n";

    // ─── Initialize legacy single-tenant TSDB (for file/TCP pipeline tap) ───
    auto tsdb_path = config.value("tsdb_path", "data/tsdb.bin");
    auto tsdb = std::make_unique<logmonitor::TSDB>(tsdb_path, g_stop);
    tsdb->start();
    std::cout << "[INFO] TSDB initialized (persistence: " << tsdb_path << ")\n";

    // ─── Initialize SaaS auth + data stores ───
    auto jwt_secret     = config.value("jwt_secret", "changeme-replace-in-production");
    auto auth_mgr       = std::make_unique<logmonitor::AuthManager>(jwt_secret);
    auto user_store     = std::make_unique<logmonitor::UserStore>("data/users.json");
    auto project_store  = std::make_unique<logmonitor::ProjectStore>("data/projects.json");

    auto default_proj_id = config.value("default_project_id", "");
    auto tenant_mgr     = std::make_unique<logmonitor::TenantManager>(
        *project_store, "data", g_stop, default_proj_id);
    tenant_mgr->start_all();

    std::cout << "[INFO] Auth + multi-tenant stores initialized\n";
    if (!default_proj_id.empty())
        std::cout << "[INFO] Default project: " << default_proj_id << "\n";

    // ─── Initialize Log Pipeline ───
    auto pipeline = std::make_unique<logmonitor::LogPipeline>(
        *ring_buffer, *thread_pool, engine, *alert_mgr, g_stop);

    // Wire pipeline tap → tenant manager (default project) + legacy TSDB
    pipeline->set_entry_callback([&tm = *tenant_mgr, &default_proj = default_proj_id,
                                   &tsdb_ref = *tsdb](
            const logmonitor::LogEntry& e,
            const logmonitor::AlertRule* rule) {
        const std::string app   = e.service_name.empty() ? e.source : e.service_name;
        const std::string level = rule ? logmonitor::severity_to_string(rule->severity) : "INFO";
        const int64_t ts_ns     = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();

        // Route to per-project TSDB if default project is configured
        if (!default_proj.empty()) {
            tm.ingest_to_project(default_proj, app, level, e.line, ts_ns);
        }

        // Always insert into legacy TSDB (powers ncurses Dashboard)
        logmonitor::TsdbEntry te;
        te.app_name     = app;
        te.level        = level;
        te.message      = e.line;
        te.timestamp_ns = ts_ns;
        tsdb_ref.insert(std::move(te));
    });

    // ─── Start File Watchers ───
    std::vector<std::shared_ptr<logmonitor::FileWatchState>> file_states;
    std::vector<std::thread> watcher_threads;

    if (config.contains("watch_files") && config["watch_files"].is_array()) {
        for (const auto& file_json : config["watch_files"]) {
            auto file_path = file_json.get<std::string>();

            auto state = std::make_shared<logmonitor::FileWatchState>();
            state->path = file_path;
            file_states.push_back(state);

            auto parent = std::filesystem::path(file_path).parent_path();
            if (!parent.empty() && !std::filesystem::exists(parent)) {
                std::filesystem::create_directories(parent);
            }
            if (!std::filesystem::exists(file_path)) {
                { std::ofstream touch(file_path); }
            }

            watcher_threads.emplace_back([&rb = *ring_buffer, file_path, state]() {
                logmonitor::FileWatcher watcher(file_path, rb, state, g_stop);
                watcher.run();
            });

            std::cout << "[INFO] Watching: " << file_path << "\n";
        }
    }

    // ─── Initialize Alert Delivery ───
    auto alert_delivery = std::make_unique<logmonitor::AlertDelivery>(5);
    std::cout << "[INFO] Alert delivery initialized (5 worker threads)\n";

    // ─── Initialize Alert Rules Engine (multi-tenant) ───
    auto alerts_json_path = config_dir / "alerts.json";
    auto rules_engine = std::make_unique<logmonitor::AlertRulesEngine>(
        *tenant_mgr, alert_delivery.get(), alerts_json_path.string(), g_stop);
    rules_engine->start();
    std::cout << "[INFO] Alert rules engine started (multi-tenant)\n";

    // ─── Start Network Ingester (legacy TCP + HTTP on port 8080) ───
    std::unique_ptr<logmonitor::NetworkIngester> network;
    if (config.contains("network") && config["network"].value("enabled", false)) {
        auto tcp_port  = config["network"].value("tcp_port", 5514);
        auto http_port = config["network"].value("http_port", 8080);
        auto max_conn  = config["network"].value("max_connections", 100);

        network = std::make_unique<logmonitor::NetworkIngester>(
            *ring_buffer,
            static_cast<uint16_t>(tcp_port),
            static_cast<uint16_t>(http_port),
            static_cast<std::size_t>(max_conn),
            g_stop);

        network->start();
    }

    // ─── Start HttpServer (multi-tenant SaaS mode) ───
    std::unique_ptr<logmonitor::HttpServer> http_server;
    if (config.contains("network")) {
        auto port_v2    = static_cast<uint16_t>(
            config["network"].value("http_port_v2", 9090));
        auto api_key    = config["network"].value("api_key", "changeme");
        auto v2_threads = static_cast<std::size_t>(
            config["network"].value("http_v2_threads", 10));

        http_server = std::make_unique<logmonitor::HttpServer>(
            *ring_buffer, port_v2, api_key, g_stop, v2_threads,
            tenant_mgr.get(), auth_mgr.get(), user_store.get(), project_store.get());

        // Legacy callbacks (power the old /query, /stats, /apps, /metrics endpoints)
        http_server->set_insert_cb([&tsdb_ref = *tsdb](
                const std::string& app, const std::string& level,
                const std::string& message, int64_t timestamp_ns) {
            logmonitor::TsdbEntry te;
            te.app_name     = app;
            te.level        = level;
            te.message      = message;
            te.timestamp_ns = timestamp_ns;
            tsdb_ref.insert(std::move(te));
        });
        http_server->set_query_cb([&tsdb_ref = *tsdb](
                const std::string& app, const std::string& level,
                int last_sec) {
            return tsdb_ref.query_to_json(app, level, last_sec);
        });
        http_server->set_stats_cb([&tsdb_ref = *tsdb](
                const std::string& app, int last_sec) {
            return tsdb_ref.stats_to_json(app, last_sec);
        });
        http_server->set_apps_cb([&tsdb_ref = *tsdb]() {
            return tsdb_ref.apps_to_json();
        });
        http_server->set_metrics_cb([&tsdb_ref = *tsdb]() {
            return tsdb_ref.metrics_to_prometheus();
        });

        // Alert rules + history callbacks
        http_server->set_rules_cb([&re = *rules_engine]() {
            return re.get_rules_json();
        });
        http_server->set_history_cb([&del = *alert_delivery]() {
            return del.history_to_json();
        });

        http_server->start();
        std::cout << "[INFO] HTTP server started on port " << port_v2 << "\n";
        std::cout << "[INFO] Dashboard: http://localhost:" << port_v2 << "\n";
    }

    // ─── Start Pipeline Consumer Thread ───
    std::thread pipeline_thread([&pipeline]() {
        pipeline->run();
    });

    // ─── Start Stats Aggregator ───
    auto stats_interval = config.value("stats_interval_seconds", 5);
    auto stats_aggregator = std::make_unique<logmonitor::StatsAggregator>(
        *pipeline, *alert_mgr, engine, network.get(), g_stop,
        "stats.json", stats_interval);

    if (network) {
        network->set_status_callback([&]() -> std::string {
            json status;
            status["status"] = "running";
            status["total_lines"] = pipeline->total_lines();
            status["total_matched"] = pipeline->total_matched();
            status["lines_per_second"] = stats_aggregator->lines_per_second();
            status["avg_latency_us"] = pipeline->avg_latency_us();
            status["alerts"]["info"] = alert_mgr->info_count();
            status["alerts"]["warn"] = alert_mgr->warn_count();
            status["alerts"]["error"] = alert_mgr->error_count();
            status["alerts"]["critical"] = alert_mgr->critical_count();
            status["watched_files"] = json::array();
            for (const auto& state : file_states) {
                json f;
                f["path"] = state->path;
                f["lines_read"] = state->lines_read.load(std::memory_order_relaxed);
                status["watched_files"].push_back(std::move(f));
            }
            if (code_indexer && code_indexer->is_built()) {
                status["code_indexer"]["files_indexed"] = code_indexer->indexed_files();
                status["code_indexer"]["tokens_indexed"] = code_indexer->index_size();
                auto locs = alert_mgr->get_last_code_locations();
                json locs_json = json::array();
                for (const auto& loc : locs) {
                    json l;
                    l["file"] = loc.file_path;
                    l["line"] = loc.line_number;
                    l["function"] = loc.function_name;
                    l["score"] = loc.relevance_score;
                    locs_json.push_back(std::move(l));
                }
                status["code_indexer"]["last_error_locations"] = std::move(locs_json);
            }
            return status.dump(2);
        });
    }

    std::thread stats_thread([&stats_aggregator]() {
        stats_aggregator->run();
    });

    // ─── Main loop / Dashboard ───
    auto refresh_ms = config.value("dashboard_refresh_ms", 500);

    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    if (!isatty(STDOUT_FILENO)) {
        std::cout << "[INFO] No TTY detected (Docker/pipe) — "
                     "running headless. Use HTTP API on port 9090.\n";
        while (!g_stop.load(std::memory_order_relaxed)) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
    } else {
        logmonitor::Dashboard dashboard(
            *pipeline, *alert_mgr, *stats_aggregator, file_states,
            network.get(), tsdb.get(), rules_engine.get(), g_stop, refresh_ms);

        dashboard.run();
    }

    // ─── Graceful Shutdown ───
    std::cout << "\n[INFO] Shutting down...\n";
    g_stop.store(true, std::memory_order_relaxed);

    if (network)     network->stop();
    if (http_server) http_server->stop();

    pipeline_thread.join();
    stats_thread.join();

    for (auto& t : watcher_threads) {
        if (t.joinable()) t.join();
    }

    thread_pool->shutdown();
    rules_engine->stop();

    // Stop tenant manager (persists all per-project TSDbs)
    tenant_mgr->stop_all();

    // Stop legacy TSDB
    tsdb->stop();

    std::cout << "[INFO] Shutdown complete. Processed "
              << pipeline->total_lines() << " lines, "
              << alert_mgr->total_alerts() << " alerts triggered.\n";

    return 0;
}
