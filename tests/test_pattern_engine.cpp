/**
 * @file test_pattern_engine.cpp
 * @brief Tests for PatternEngine: rule loading, matching, severity counters.
 */

#include "PatternEngine.hpp"

#include <nlohmann/json.hpp>

#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

using namespace logmonitor;

// ─── Test 1: Manual rule add and match ───

void test_manual_rules() {
    PatternEngine engine;
    engine.add_rule("errors", "ERROR|Exception", Severity::ERROR);
    engine.add_rule("warnings", "WARN|warning", Severity::WARN);

    assert(engine.rule_count() == 2);

    auto* rule = engine.match("2026-03-27 ERROR: something failed");
    assert(rule != nullptr);
    assert(rule->name == "errors");
    assert(rule->severity == Severity::ERROR);

    auto* rule2 = engine.match("WARN: disk usage high");
    assert(rule2 != nullptr);
    assert(rule2->name == "warnings");

    auto* no_match = engine.match("INFO: system started normally");
    assert(no_match == nullptr);

    std::cout << "  [PASS] test_manual_rules\n";
}

// ─── Test 2: First-match-wins ordering ───

void test_first_match_wins() {
    PatternEngine engine;
    engine.add_rule("critical", "CRITICAL|segfault", Severity::CRITICAL);
    engine.add_rule("errors", "ERROR|failed", Severity::ERROR);

    // "CRITICAL ERROR" should match critical first
    auto* rule = engine.match("CRITICAL ERROR: segfault in module X");
    assert(rule != nullptr);
    assert(rule->name == "critical");
    assert(rule->severity == Severity::CRITICAL);

    std::cout << "  [PASS] test_first_match_wins\n";
}

// ─── Test 3: Atomic trigger counter ───

void test_trigger_counter() {
    PatternEngine engine;
    engine.add_rule("errors", "ERROR", Severity::ERROR);

    auto* rule = engine.match("ERROR: first");
    assert(rule != nullptr);
    rule->trigger_count.fetch_add(1, std::memory_order_relaxed);

    rule = engine.match("ERROR: second");
    assert(rule != nullptr);
    rule->trigger_count.fetch_add(1, std::memory_order_relaxed);

    assert(engine.rules()[0]->trigger_count.load() == 2);

    std::cout << "  [PASS] test_trigger_counter\n";
}

// ─── Test 4: Load rules from config file ───

void test_load_from_config() {
    // Create a temporary config file
    std::string temp_config = "/tmp/test_logmonitor_config.json";

    nlohmann::json config;
    config["rules"] = nlohmann::json::array({
        {{"name", "test_critical"}, {"pattern", "CRITICAL|OOM"}, {"severity", "CRITICAL"}},
        {{"name", "test_error"}, {"pattern", "ERROR|Exception"}, {"severity", "ERROR"}},
        {{"name", "test_info"}, {"pattern", "INFO|started"}, {"severity", "INFO"}}
    });

    {
        std::ofstream f(temp_config);
        f << config.dump(2);
    }

    PatternEngine engine;
    load_rules_from_config(engine, temp_config);

    assert(engine.rule_count() == 3);

    auto* r1 = engine.match("CRITICAL: OOM killer invoked");
    assert(r1 && r1->severity == Severity::CRITICAL);

    auto* r2 = engine.match("ERROR: NullPointerException");
    assert(r2 && r2->severity == Severity::ERROR);

    auto* r3 = engine.match("INFO: server started");
    assert(r3 && r3->severity == Severity::INFO);

    // Cleanup
    std::filesystem::remove(temp_config);

    std::cout << "  [PASS] test_load_from_config\n";
}

// ─── Test 5: Severity enum conversions ───

void test_severity_conversion() {
    assert(severity_from_string("CRITICAL") == Severity::CRITICAL);
    assert(severity_from_string("ERROR") == Severity::ERROR);
    assert(severity_from_string("WARN") == Severity::WARN);
    assert(severity_from_string("INFO") == Severity::INFO);
    assert(severity_from_string("unknown_stuff") == Severity::INFO);  // default

    assert(std::string(severity_to_string(Severity::CRITICAL)) == "CRITICAL");
    assert(std::string(severity_to_string(Severity::ERROR)) == "ERROR");
    assert(std::string(severity_to_string(Severity::WARN)) == "WARN");
    assert(std::string(severity_to_string(Severity::INFO)) == "INFO");

    std::cout << "  [PASS] test_severity_conversion\n";
}

// ─── Test 6: Empty engine returns no match ───

void test_empty_engine() {
    PatternEngine engine;
    assert(engine.rule_count() == 0);
    assert(engine.match("ERROR: something") == nullptr);

    std::cout << "  [PASS] test_empty_engine\n";
}

// ─── Test 7: Regex special characters ───

void test_regex_patterns() {
    PatternEngine engine;
    engine.add_rule("ip_match", R"(\d{1,3}\.\d{1,3}\.\d{1,3}\.\d{1,3})", Severity::INFO);

    auto* rule = engine.match("Connection from 192.168.1.100");
    assert(rule != nullptr);

    auto* no_match = engine.match("No IP address here");
    assert(no_match == nullptr);

    std::cout << "  [PASS] test_regex_patterns\n";
}

int main() {
    std::cout << "=== Pattern Engine Tests ===\n";

    test_manual_rules();
    test_first_match_wins();
    test_trigger_counter();
    test_load_from_config();
    test_severity_conversion();
    test_empty_engine();
    test_regex_patterns();

    std::cout << "=== All Pattern Engine Tests PASSED ===\n";
    return 0;
}
