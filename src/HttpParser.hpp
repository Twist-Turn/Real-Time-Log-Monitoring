#pragma once
/**
 * @file HttpParser.hpp
 * @brief Minimal HTTP/1.1 request parser for the log ingestion REST API.
 *
 * Parses: request line (method, path, version), headers, and body.
 * Only supports what we need: POST /ingest and GET /status.
 * ~150 lines, header-only, zero external dependencies.
 */

#include <algorithm>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>

namespace logmonitor {

struct HttpRequest {
    std::string method;    // "GET", "POST"
    std::string path;      // "/ingest", "/status"
    std::string version;   // "HTTP/1.1"
    std::unordered_map<std::string, std::string> headers;
    std::string body;

    /// Get a header value (case-insensitive lookup, returns lowercase keys)
    [[nodiscard]] std::optional<std::string> header(const std::string& key) const {
        std::string lower_key = key;
        std::transform(lower_key.begin(), lower_key.end(), lower_key.begin(), ::tolower);
        auto it = headers.find(lower_key);
        if (it != headers.end()) return it->second;
        return std::nullopt;
    }

    /// Get Content-Length, or 0 if not present
    [[nodiscard]] std::size_t content_length() const {
        auto cl = header("content-length");
        if (cl) {
            try { return std::stoull(*cl); }
            catch (...) { return 0; }
        }
        return 0;
    }
};

struct HttpResponse {
    int status_code = 200;
    std::string status_text = "OK";
    std::string content_type = "application/json";
    std::string body;

    /// Serialize to HTTP/1.1 response string
    [[nodiscard]] std::string serialize() const {
        std::ostringstream oss;
        oss << "HTTP/1.1 " << status_code << " " << status_text << "\r\n"
            << "Content-Type: " << content_type << "\r\n"
            << "Content-Length: " << body.size() << "\r\n"
            << "Connection: close\r\n"
            << "\r\n"
            << body;
        return oss.str();
    }

    static HttpResponse ok(const std::string& json_body) {
        return {200, "OK", "application/json", json_body};
    }

    static HttpResponse bad_request(const std::string& msg) {
        return {400, "Bad Request", "application/json",
                "{\"error\":\"" + msg + "\"}"};
    }

    static HttpResponse not_found() {
        return {404, "Not Found", "application/json",
                "{\"error\":\"Not Found\"}"};
    }

    static HttpResponse server_error(const std::string& msg) {
        return {500, "Internal Server Error", "application/json",
                "{\"error\":\"" + msg + "\"}"};
    }

    static HttpResponse html(const std::string& html_body) {
        return {200, "OK", "text/html; charset=utf-8", html_body};
    }
};

class HttpParser {
public:
    /**
     * @brief Parse a raw HTTP request from a buffer.
     * @param raw The complete raw HTTP request bytes.
     * @return Parsed HttpRequest, or std::nullopt on malformed input.
     */
    static std::optional<HttpRequest> parse(const std::string& raw) {
        HttpRequest req;

        // Find the end of headers (\r\n\r\n)
        auto header_end = raw.find("\r\n\r\n");
        if (header_end == std::string::npos) {
            // Try just \n\n as fallback
            header_end = raw.find("\n\n");
            if (header_end == std::string::npos) return std::nullopt;
        }

        std::string_view header_section(raw.data(), header_end);

        // Parse request line
        auto first_line_end = header_section.find('\n');
        if (first_line_end == std::string_view::npos) return std::nullopt;

        std::string_view request_line = header_section.substr(0, first_line_end);
        // Trim \r
        if (!request_line.empty() && request_line.back() == '\r') {
            request_line.remove_suffix(1);
        }

        // Split request line: "GET /path HTTP/1.1"
        auto sp1 = request_line.find(' ');
        if (sp1 == std::string_view::npos) return std::nullopt;
        auto sp2 = request_line.find(' ', sp1 + 1);
        if (sp2 == std::string_view::npos) return std::nullopt;

        req.method = std::string(request_line.substr(0, sp1));
        req.path = std::string(request_line.substr(sp1 + 1, sp2 - sp1 - 1));
        req.version = std::string(request_line.substr(sp2 + 1));

        // Parse headers
        std::string_view remaining = header_section.substr(first_line_end + 1);
        while (!remaining.empty()) {
            auto line_end = remaining.find('\n');
            std::string_view line = (line_end != std::string_view::npos)
                ? remaining.substr(0, line_end)
                : remaining;

            // Trim \r
            if (!line.empty() && line.back() == '\r') {
                line.remove_suffix(1);
            }

            if (!line.empty()) {
                auto colon = line.find(':');
                if (colon != std::string_view::npos) {
                    std::string key(line.substr(0, colon));
                    std::transform(key.begin(), key.end(), key.begin(), ::tolower);

                    std::string_view value = line.substr(colon + 1);
                    // Trim leading whitespace from value
                    while (!value.empty() && value.front() == ' ') {
                        value.remove_prefix(1);
                    }
                    req.headers[key] = std::string(value);
                }
            }

            if (line_end == std::string_view::npos) break;
            remaining = remaining.substr(line_end + 1);
        }

        // Extract body (everything after header separator)
        std::size_t body_start = header_end +
            (raw[header_end] == '\r' ? 4 : 2);  // \r\n\r\n vs \n\n
        if (body_start < raw.size()) {
            req.body = raw.substr(body_start);
        }

        return req;
    }
};

} // namespace logmonitor
