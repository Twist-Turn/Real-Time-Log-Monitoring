/**
 * @file PatternEngine.cpp
 * @brief Implementation of config-based rule loading.
 *
 * Kept separate from the header so that nlohmann/json is only
 * included in translation units that need it.
 */

#include "PatternEngine.hpp"
#include <nlohmann/json.hpp>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace logmonitor {

using json = nlohmann::json;

/// Load pattern rules from a JSON config file
void load_rules_from_config(PatternEngine& engine, const std::filesystem::path& config_path) {
    if (!std::filesystem::exists(config_path)) {
        throw std::runtime_error("Config file not found: " + config_path.string());
    }

    std::ifstream file(config_path);
    if (!file.is_open()) {
        throw std::runtime_error("Cannot open config file: " + config_path.string());
    }

    json config;
    try {
        file >> config;
    } catch (const json::parse_error& e) {
        throw std::runtime_error(
            "JSON parse error in " + config_path.string() + ": " + e.what());
    }

    if (!config.contains("rules") || !config["rules"].is_array()) {
        throw std::runtime_error("Config must contain a 'rules' array");
    }

    for (const auto& rule_json : config["rules"]) {
        if (!rule_json.contains("name") || !rule_json.contains("pattern") ||
            !rule_json.contains("severity")) {
            std::cerr << "[WARN] Skipping malformed rule in config\n";
            continue;
        }

        auto name     = rule_json["name"].get<std::string>();
        auto pattern  = rule_json["pattern"].get<std::string>();
        auto severity = severity_from_string(rule_json["severity"].get<std::string>());

        try {
            engine.add_rule(std::move(name), pattern, severity);
        } catch (const std::regex_error& e) {
            std::cerr << "[WARN] Invalid regex in rule '" << name
                      << "': " << e.what() << "\n";
        }
    }

    std::cout << "[INFO] Loaded " << engine.rule_count()
              << " alert rules from " << config_path.string() << "\n";
}

} // namespace logmonitor
