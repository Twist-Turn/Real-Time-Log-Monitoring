#pragma once
/**
 * @file AlertDelivery.hpp
 * @brief Async alert delivery via HTTP webhooks and SMTP email.
 *
 * Features:
 *   - HTTP POST to notify_url via libcurl
 *   - 3 retries with exponential backoff: 1s, 2s, 4s
 *   - Dedicated thread pool of 5 workers (non-blocking for main system)
 *   - Alert history: last 100 fired alerts in memory
 *   - Discord, Slack, and generic webhook format support
 *   - SMTP email support (notify_url = "smtp://..." or "smtps://...")
 *
 * Usage:
 *   AlertDelivery delivery;
 *   delivery.send(fired_alert);       // async, returns immediately
 *   auto history = delivery.get_history();  // GET /alerts/history
 */

#define LOGMONITOR_ALERT_DELIVERY_INCLUDED

#include "AlertRulesEngine.hpp"  // FiredAlert, AlertRulesEngine
#include "ThreadPool.hpp"

#include <nlohmann/json.hpp>

// libcurl
#include <curl/curl.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace logmonitor {

// ─── AlertDelivery ───

class AlertDelivery {
public:
    static constexpr std::size_t MAX_HISTORY = 100;
    static constexpr int         MAX_RETRIES = 3;
    static constexpr long        CURL_TIMEOUT_S = 10;

    explicit AlertDelivery(std::size_t thread_count = 5)
        : pool_(thread_count)
    {
        curl_global_init(CURL_GLOBAL_DEFAULT);
    }

    ~AlertDelivery() {
        pool_.shutdown();
        curl_global_cleanup();
    }

    /// Enqueue an alert for async delivery.
    void send(FiredAlert alert) {
        pool_.enqueue([this, alert = std::move(alert)]() mutable {
            deliver(alert);
            // Store in history regardless of delivery outcome
            std::lock_guard<std::mutex> lock(history_mutex_);
            history_.push_back(alert);
            while (history_.size() > MAX_HISTORY) {
                history_.pop_front();
            }
        });
    }

    /// Returns up to the last 100 fired alerts.
    std::vector<FiredAlert> get_history() const {
        std::lock_guard<std::mutex> lock(history_mutex_);
        return std::vector<FiredAlert>(history_.begin(), history_.end());
    }

    /// JSON for GET /alerts/history
    std::string history_to_json() const {
        auto history = get_history();
        nlohmann::json arr = nlohmann::json::array();
        for (const auto& a : history) {
            nlohmann::json obj;
            obj["rule_id"]        = a.rule_id;
            obj["app"]            = a.app;
            obj["level"]          = a.level;
            obj["count"]          = a.count;
            obj["threshold"]      = a.threshold;
            obj["window_seconds"] = a.window_seconds;
            obj["fired_at_ns"]    = a.fired_at_ns;
            obj["message"]        = a.message;
            obj["delivered"]      = a.delivered;
            if (!a.delivery_error.empty()) {
                obj["error"] = a.delivery_error;
            }
            arr.push_back(std::move(obj));
        }
        nlohmann::json result;
        result["history"] = arr;
        return result.dump();
    }

private:
    // ─── Webhook type detection ───
    enum class WebhookType { GENERIC, SLACK, DISCORD, SMTP };

    static WebhookType detect_type(const std::string& url) {
        if (url.find("hooks.slack.com") != std::string::npos) return WebhookType::SLACK;
        if (url.find("discord.com/api/webhooks") != std::string::npos) return WebhookType::DISCORD;
        if (url.substr(0, 7) == "smtp://" ||
            url.substr(0, 8) == "smtps://") return WebhookType::SMTP;
        return WebhookType::GENERIC;
    }

    // ─── Payload formatters ───

    static std::string format_generic(const FiredAlert& a) {
        nlohmann::json j;
        j["rule_id"]        = a.rule_id;
        j["app"]            = a.app;
        j["level"]          = a.level;
        j["count"]          = a.count;
        j["threshold"]      = a.threshold;
        j["window_seconds"] = a.window_seconds;
        j["fired_at"]       = a.fired_at_ns / 1'000'000'000LL;  // Unix seconds
        j["message"]        = a.message;
        return j.dump();
    }

    static std::string format_slack(const FiredAlert& a) {
        nlohmann::json j;
        j["text"] = ":rotating_light: *ALERT*: " + a.message;
        nlohmann::json attach;
        attach["color"]  = (a.level == "ERROR" || a.level == "CRITICAL") ? "danger" : "warning";
        attach["fields"] = nlohmann::json::array({
            {{"title","Rule"},{"value",a.rule_id},{"short",true}},
            {{"title","App"}, {"value",a.app},    {"short",true}},
            {{"title","Level"},{"value",a.level}, {"short",true}},
            {{"title","Count"},{"value",std::to_string(a.count)},{"short",true}}
        });
        j["attachments"] = nlohmann::json::array({attach});
        return j.dump();
    }

    static std::string format_discord(const FiredAlert& a) {
        nlohmann::json j;
        j["content"] = "**ALERT**: " + a.message;
        nlohmann::json embed;
        embed["title"]       = "Rule: " + a.rule_id;
        embed["description"] = a.message;
        embed["color"]       = (a.level == "CRITICAL") ? 0xCC0000 : 0xFF6600;
        nlohmann::json field_app;
        field_app["name"]   = "App";
        field_app["value"]  = a.app;
        field_app["inline"] = true;
        nlohmann::json field_level;
        field_level["name"]   = "Level";
        field_level["value"]  = a.level;
        field_level["inline"] = true;
        embed["fields"] = nlohmann::json::array({field_app, field_level});
        j["embeds"] = nlohmann::json::array({embed});
        return j.dump();
    }

    // ─── curl helper ───

    static size_t curl_write_cb(void* /*ptr*/, size_t size, size_t nmemb,
                                 void* /*userdata*/) {
        return size * nmemb;  // Discard response body
    }

    bool send_via_curl(const std::string& url, const std::string& body,
                       const std::string& content_type) {
        CURL* curl = curl_easy_init();
        if (!curl) return false;

        struct curl_slist* headers = nullptr;
        std::string ct_header = "Content-Type: " + content_type;
        headers = curl_slist_append(headers, ct_header.c_str());

        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE,
                         static_cast<long>(body.size()));
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, CURL_TIMEOUT_S);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);

        CURLcode rc = curl_easy_perform(curl);

        long http_code = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);

        return rc == CURLE_OK && http_code >= 200 && http_code < 300;
    }

    bool send_smtp(const std::string& smtp_url, const FiredAlert& a) {
        // Minimal SMTP: expects smtp://user:pass@host:port/
        // Subject and body constructed inline.
        CURL* curl = curl_easy_init();
        if (!curl) return false;

        std::string payload =
            "Subject: [LogMonitor] Alert: " + a.rule_id + "\r\n"
            "Content-Type: text/plain\r\n\r\n" + a.message + "\r\n";

        // Use a simple read callback from a string
        struct ReadState {
            const std::string& data;
            std::size_t offset{0};
        };
        ReadState state{payload};

        curl_easy_setopt(curl, CURLOPT_URL, smtp_url.c_str());
        curl_easy_setopt(curl, CURLOPT_UPLOAD, 1L);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, CURL_TIMEOUT_S);
        curl_easy_setopt(curl, CURLOPT_READFUNCTION,
            +[](void* ptr, size_t size, size_t nmemb, void* udata) -> size_t {
                auto* s = static_cast<ReadState*>(udata);
                std::size_t to_copy = std::min(size * nmemb,
                                               s->data.size() - s->offset);
                if (to_copy == 0) return 0;
                std::memcpy(ptr, s->data.data() + s->offset, to_copy);
                s->offset += to_copy;
                return to_copy;
            });
        curl_easy_setopt(curl, CURLOPT_READDATA, &state);

        CURLcode rc = curl_easy_perform(curl);
        curl_easy_cleanup(curl);
        return rc == CURLE_OK;
    }

    // ─── Delivery with retry ───

    void deliver(FiredAlert& alert) {
        if (alert.notify_url.empty()) {
            alert.delivered = true;  // No-op, not an error
            return;
        }

        auto type = detect_type(alert.notify_url);
        bool success = false;

        for (int attempt = 0; attempt < MAX_RETRIES && !success; ++attempt) {
            if (attempt > 0) {
                int sleep_seconds = 1 << (attempt - 1);  // 1s, 2s, 4s
                std::this_thread::sleep_for(std::chrono::seconds(sleep_seconds));
            }

            try {
                switch (type) {
                    case WebhookType::SLACK:
                        success = send_via_curl(alert.notify_url,
                                                format_slack(alert),
                                                "application/json");
                        break;
                    case WebhookType::DISCORD:
                        success = send_via_curl(alert.notify_url,
                                                format_discord(alert),
                                                "application/json");
                        break;
                    case WebhookType::SMTP:
                        success = send_smtp(alert.notify_url, alert);
                        break;
                    case WebhookType::GENERIC:
                        success = send_via_curl(alert.notify_url,
                                                format_generic(alert),
                                                "application/json");
                        break;
                }
            } catch (const std::exception& ex) {
                alert.delivery_error = ex.what();
            }
        }

        alert.delivered = success;
        if (!success) {
            std::cerr << "[AlertDelivery] Failed to deliver alert "
                      << alert.rule_id << " to " << alert.notify_url << "\n";
        } else {
            std::cout << "[AlertDelivery] Delivered alert "
                      << alert.rule_id << " → " << alert.notify_url << "\n";
        }
    }

    // ─── Members ───
    ThreadPool pool_;
    mutable std::mutex history_mutex_;
    std::deque<FiredAlert> history_;
};

// ─── Provide real dispatch_alert implementation (replaces stub in AlertRulesEngine) ───
inline void AlertRulesEngine::dispatch_alert(const FiredAlert& alert) {
    if (delivery_) {
        delivery_->send(alert);
    }
}

} // namespace logmonitor
