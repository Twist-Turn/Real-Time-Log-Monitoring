#pragma once
/**
 * @file NetworkIngester.hpp
 * @brief TCP + HTTP server for remote log ingestion.
 *
 * Allows any external service to send logs to this system:
 *   - TCP (port 5514): line-delimited "service|severity|message\n"
 *   - HTTP (port 8080): POST /ingest with JSON body, GET /status
 *
 * Both push LogEntry into the same shared MPSC RingBuffer as FileWatchers.
 */

#include "FileWatcher.hpp"    // LogRingBuffer
#include "HttpParser.hpp"
#include "WebDashboard.hpp"
#include "PatternEngine.hpp"  // LogEntry
#include "TcpSocket.hpp"
#include "ThreadPool.hpp"

#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <functional>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

namespace logmonitor {

/// Information about a connected network client
struct ConnectionInfo {
    std::string service_name;
    std::string remote_addr;
    std::chrono::system_clock::time_point connected_at;
    std::atomic<uint64_t> lines_received{0};
};

class NetworkIngester {
public:
    /// Callback type for generating status JSON (injected from main)
    using StatusCallback = std::function<std::string()>;

    NetworkIngester(LogRingBuffer& ring_buffer,
                    uint16_t tcp_port,
                    uint16_t http_port,
                    std::size_t max_connections,
                    std::atomic<bool>& stop_flag)
        : ring_buffer_(ring_buffer)
        , tcp_port_(tcp_port)
        , http_port_(http_port)
        , max_connections_(max_connections)
        , stop_flag_(stop_flag)
        , pool_(std::min(max_connections, std::size_t(8)))  // Limit pool size
    {}

    ~NetworkIngester() {
        stop();
    }

    /// Set callback for GET /status endpoint
    void set_status_callback(StatusCallback cb) {
        status_callback_ = std::move(cb);
    }

    /// Start TCP and HTTP acceptor threads
    void start() {
        tcp_thread_ = std::thread([this] { run_tcp_server(); });
        http_thread_ = std::thread([this] { run_http_server(); });
    }

    /// Stop all network threads
    void stop() {
        pool_.shutdown();
        if (tcp_thread_.joinable()) tcp_thread_.join();
        if (http_thread_.joinable()) http_thread_.join();
    }

    /// Get count of active connections
    [[nodiscard]] std::size_t active_connections() const {
        std::lock_guard<std::mutex> lock(connections_mutex_);
        return connections_.size();
    }

    /// Get total lines received over network
    [[nodiscard]] uint64_t total_network_lines() const {
        return total_network_lines_.load(std::memory_order_relaxed);
    }

    /// Get connection info snapshot (for dashboard/stats)
    [[nodiscard]] std::vector<std::pair<std::string, uint64_t>> get_connections() const {
        std::lock_guard<std::mutex> lock(connections_mutex_);
        std::vector<std::pair<std::string, uint64_t>> result;
        for (const auto& [id, info] : connections_) {
            result.emplace_back(
                info->service_name.empty() ? info->remote_addr : info->service_name,
                info->lines_received.load(std::memory_order_relaxed));
        }
        return result;
    }

private:
    // ─── TCP Server ───

    void run_tcp_server() {
        try {
            auto server = TcpSocket::create();
            server.set_reuse_addr();
            server.set_nonblocking();
            server.bind(tcp_port_);
            server.listen(128);

            std::cout << "[INFO] TCP log server listening on port " << tcp_port_ << "\n";

            while (!stop_flag_.load(std::memory_order_relaxed)) {
                auto client = server.accept();
                if (!client.valid()) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(50));
                    continue;
                }

                // Track connection
                std::string conn_id = "tcp:" + std::to_string(client.fd());
                auto info = std::make_shared<ConnectionInfo>();
                info->remote_addr = conn_id;
                info->connected_at = std::chrono::system_clock::now();
                {
                    std::lock_guard<std::mutex> lock(connections_mutex_);
                    connections_[conn_id] = info;
                }

                // Handle in thread pool
                int client_fd = client.release();
                pool_.enqueue([this, client_fd, conn_id, info]() {
                    handle_tcp_client(TcpSocket(client_fd), conn_id, info);
                });
            }
        } catch (const std::exception& e) {
            std::cerr << "[ERROR] TCP server failed: " << e.what() << "\n";
        }
    }

    void handle_tcp_client(TcpSocket client, const std::string& conn_id,
                           std::shared_ptr<ConnectionInfo> info) {
        // Read lines until connection closes or shutdown
        while (!stop_flag_.load(std::memory_order_relaxed)) {
            std::string line = client.recv_line();
            if (line.empty()) break;  // EOF or error

            // Parse: "service_name|severity|message"
            std::string service_name;
            std::string message = line;

            auto first_pipe = line.find('|');
            if (first_pipe != std::string::npos) {
                service_name = line.substr(0, first_pipe);
                auto second_pipe = line.find('|', first_pipe + 1);
                if (second_pipe != std::string::npos) {
                    // severity is between pipes (we don't use it here — PatternEngine handles it)
                    message = line.substr(second_pipe + 1);
                } else {
                    message = line.substr(first_pipe + 1);
                }
            }

            if (!service_name.empty()) {
                info->service_name = service_name;
            }

            LogEntry entry;
            entry.line = std::move(message);
            entry.source = conn_id;
            entry.service_name = service_name;
            entry.timestamp = std::chrono::steady_clock::now();
            entry.line_number = info->lines_received.fetch_add(1, std::memory_order_relaxed) + 1;

            total_network_lines_.fetch_add(1, std::memory_order_relaxed);

            // Push to ring buffer (retry on full)
            int retries = 0;
            while (!ring_buffer_.try_push(std::move(entry))) {
                if (stop_flag_.load(std::memory_order_relaxed)) return;
                if (++retries > 100) {
                    std::this_thread::yield();
                    retries = 0;
                }
            }
        }

        // Remove connection tracking
        {
            std::lock_guard<std::mutex> lock(connections_mutex_);
            connections_.erase(conn_id);
        }
    }

    // ─── HTTP Server ───

    void run_http_server() {
        try {
            auto server = TcpSocket::create();
            server.set_reuse_addr();
            server.set_nonblocking();
            server.bind(http_port_);
            server.listen(128);

            std::cout << "[INFO] HTTP log server listening on port " << http_port_ << "\n";

            while (!stop_flag_.load(std::memory_order_relaxed)) {
                auto client = server.accept();
                if (!client.valid()) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(50));
                    continue;
                }

                int client_fd = client.release();
                pool_.enqueue([this, client_fd]() {
                    handle_http_client(TcpSocket(client_fd));
                });
            }
        } catch (const std::exception& e) {
            std::cerr << "[ERROR] HTTP server failed: " << e.what() << "\n";
        }
    }

    void handle_http_client(TcpSocket client) {
        // Read the full request (up to 64KB)
        std::string raw_request;
        raw_request.reserve(4096);

        // Read until we have headers + body
        for (int i = 0; i < 100; ++i) {
            auto chunk = client.recv(4096);
            if (chunk.empty()) break;
            raw_request += chunk;

            // Check if we have the full request
            auto header_end = raw_request.find("\r\n\r\n");
            if (header_end != std::string::npos) {
                // Check Content-Length
                auto cl_pos = raw_request.find("Content-Length:");
                if (cl_pos == std::string::npos) {
                    cl_pos = raw_request.find("content-length:");
                }
                if (cl_pos != std::string::npos) {
                    auto cl_end = raw_request.find("\r\n", cl_pos);
                    auto cl_val = raw_request.substr(cl_pos + 16, cl_end - cl_pos - 16);
                    std::size_t content_length = 0;
                    try { content_length = std::stoull(cl_val); } catch (...) {}
                    std::size_t body_start = header_end + 4;
                    if (raw_request.size() >= body_start + content_length) break;
                } else {
                    break;  // No body expected
                }
            }
        }

        auto parsed = HttpParser::parse(raw_request);
        if (!parsed) {
            auto resp = HttpResponse::bad_request("Malformed HTTP request");
            client.send_all(resp.serialize());
            return;
        }

        HttpResponse response;

        if (parsed->method == "POST" && parsed->path == "/ingest") {
            response = handle_ingest(*parsed);
        } else if (parsed->method == "GET" && parsed->path == "/status") {
            response = handle_status();
        } else if (parsed->method == "GET" && parsed->path == "/health") {
            response = HttpResponse::ok("{\"status\":\"ok\"}");
        } else if (parsed->method == "GET" &&
                   (parsed->path == "/dashboard" || parsed->path == "/")) {
            response = HttpResponse::html(web_dashboard_html());
        } else {
            response = HttpResponse::not_found();
        }

        client.send_all(response.serialize());
    }

    /// Handle POST /ingest
    HttpResponse handle_ingest(const HttpRequest& req) {
        try {
            auto json = nlohmann::json::parse(req.body);
            auto service = json.value("service", "unknown");
            uint64_t count = 0;

            if (json.contains("lines") && json["lines"].is_array()) {
                for (const auto& line_json : json["lines"]) {
                    LogEntry entry;
                    entry.line = line_json.get<std::string>();
                    entry.source = "http:" + service;
                    entry.service_name = service;
                    entry.timestamp = std::chrono::steady_clock::now();
                    entry.line_number = total_network_lines_.fetch_add(1, std::memory_order_relaxed) + 1;

                    // Push to ring buffer
                    int retries = 0;
                    while (!ring_buffer_.try_push(std::move(entry))) {
                        if (stop_flag_.load(std::memory_order_relaxed)) break;
                        if (++retries > 100) {
                            std::this_thread::yield();
                            retries = 0;
                        }
                    }
                    ++count;
                }
            } else if (json.contains("line")) {
                // Single line mode
                LogEntry entry;
                entry.line = json["line"].get<std::string>();
                entry.source = "http:" + service;
                entry.service_name = service;
                entry.timestamp = std::chrono::steady_clock::now();
                entry.line_number = total_network_lines_.fetch_add(1, std::memory_order_relaxed) + 1;

                ring_buffer_.try_push(std::move(entry));
                count = 1;
            }

            return HttpResponse::ok(
                "{\"status\":\"ok\",\"ingested\":" + std::to_string(count) + "}");
        } catch (const std::exception& e) {
            return HttpResponse::bad_request(std::string("JSON parse error: ") + e.what());
        }
    }

    /// Handle GET /status
    HttpResponse handle_status() {
        if (status_callback_) {
            return HttpResponse::ok(status_callback_());
        }
        return HttpResponse::ok("{\"status\":\"running\"}");
    }

    // ─── Data ───
    LogRingBuffer& ring_buffer_;
    uint16_t tcp_port_;
    uint16_t http_port_;
    [[maybe_unused]] std::size_t max_connections_;
    std::atomic<bool>& stop_flag_;

    ThreadPool pool_;
    std::thread tcp_thread_;
    std::thread http_thread_;

    mutable std::mutex connections_mutex_;
    std::unordered_map<std::string, std::shared_ptr<ConnectionInfo>> connections_;
    std::atomic<uint64_t> total_network_lines_{0};

    StatusCallback status_callback_;
};

} // namespace logmonitor
