#pragma once
/**
 * @file PatternEngine.hpp
 * @brief Regex-based log line pattern matcher with severity classification.
 *
 * Loads alert rules from JSON config. Each rule has a regex pattern, severity
 * level, and an atomic hit counter. Rules are immutable after construction
 * (thread-safe for concurrent reads). The atomic counters are the only
 * mutable state and are safe for concurrent increments.
 */

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <regex>
#include <string>
#include <string_view>
#include <vector>

namespace logmonitor {

// ─── Shared types used across the entire system ───

enum class Severity : uint8_t {
    INFO     = 0,
    WARN     = 1,
    ERROR    = 2,
    CRITICAL = 3
};

/// Convert severity enum to string
inline const char* severity_to_string(Severity s) {
    switch (s) {
        case Severity::INFO:     return "INFO";
        case Severity::WARN:     return "WARN";
        case Severity::ERROR:    return "ERROR";
        case Severity::CRITICAL: return "CRITICAL";
    }
    return "UNKNOWN";
}

/// Parse severity from string (case-insensitive)
inline Severity severity_from_string(std::string_view s) {
    if (s == "CRITICAL" || s == "critical") return Severity::CRITICAL;
    if (s == "ERROR"    || s == "error")    return Severity::ERROR;
    if (s == "WARN"     || s == "warn")     return Severity::WARN;
    return Severity::INFO;
}

/// A single log entry flowing through the pipeline
struct LogEntry {
    std::string line;
    std::string source;          // file path or "tcp:addr" or "http:service"
    std::string service_name;    // optional: name from network client
    std::chrono::steady_clock::time_point timestamp;
    uint64_t line_number{0};
};

/// An alert rule loaded from config
struct AlertRule {
    std::string name;
    std::regex pattern;
    std::string pattern_str;  // Original pattern string (for display/serialization)
    Severity severity;
    std::atomic<uint64_t> trigger_count{0};

    // Non-copyable due to atomic, but we store in shared_ptr anyway
    AlertRule() = default;
    AlertRule(std::string n, const std::string& pat, Severity sev)
        : name(std::move(n))
        , pattern(pat, std::regex::optimize | std::regex::ECMAScript)
        , pattern_str(pat)
        , severity(sev)
    {}
};

/// Result of a code location lookup (RAG)
struct CodeLocation {
    std::string file_path;
    int line_number{0};
    std::string function_name;
    std::string snippet;        // Surrounding lines of code
    float relevance_score{0.0f};
};

// ─── Pattern Engine ───

class PatternEngine {
public:
    PatternEngine() = default;

    /// Add a rule manually
    void add_rule(std::string name, const std::string& pattern, Severity severity) {
        rules_.push_back(
            std::make_shared<AlertRule>(std::move(name), pattern, severity));
    }

    /**
     * @brief Match a log line against all rules.
     * @return Pointer to the first matching rule, or nullptr if no match.
     *
     * Rules are checked in order; first match wins. The raw pointer is safe
     * because rules_ is immutable after initialization and the shared_ptrs
     * keep them alive for the lifetime of the engine.
     */
    AlertRule* match(std::string_view line) const {
        // std::regex_search requires iterators, so we need to work with
        // the underlying data. string_view → pointer range.
        const auto* begin = line.data();
        const auto* end   = begin + line.size();

        for (const auto& rule : rules_) {
            if (std::regex_search(begin, end, rule->pattern)) {
                return rule.get();
            }
        }
        return nullptr;
    }

    [[nodiscard]] const std::vector<std::shared_ptr<AlertRule>>& rules() const {
        return rules_;
    }

    [[nodiscard]] std::size_t rule_count() const { return rules_.size(); }

private:
    std::vector<std::shared_ptr<AlertRule>> rules_;
};

/// Load pattern rules from a JSON config file (defined in PatternEngine.cpp)
void load_rules_from_config(PatternEngine& engine, const std::filesystem::path& config_path);

} // namespace logmonitor
