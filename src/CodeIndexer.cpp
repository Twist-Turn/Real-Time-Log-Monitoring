/**
 * @file CodeIndexer.cpp
 * @brief CodeIndexer config loading and utility functions.
 */

#include "CodeIndexer.hpp"
#include <nlohmann/json.hpp>
#include <iostream>

namespace logmonitor {

using json = nlohmann::json;

/// Build CodeIndexer from config JSON
std::unique_ptr<CodeIndexer> create_code_indexer_from_config(
        const json& config,
        const std::filesystem::path& config_dir) {
    if (!config.contains("codebase_index") ||
        !config["codebase_index"].value("enabled", false)) {
        std::cout << "[INFO] Code indexer disabled in config\n";
        return nullptr;
    }

    const auto& ci_config = config["codebase_index"];
    auto path_str = ci_config.value("path", "");
    if (path_str.empty()) {
        std::cerr << "[WARN] codebase_index.path is empty, skipping\n";
        return nullptr;
    }

    // Resolve relative paths against config directory
    std::filesystem::path codebase_path(path_str);
    if (codebase_path.is_relative()) {
        codebase_path = config_dir / codebase_path;
    }

    auto max_results = ci_config.value("max_results", 3);

    std::vector<std::string> extensions;
    if (ci_config.contains("extensions") && ci_config["extensions"].is_array()) {
        for (const auto& ext : ci_config["extensions"]) {
            extensions.push_back(ext.get<std::string>());
        }
    } else {
        // Default extensions
        extensions = {".cpp", ".hpp", ".py", ".java", ".js", ".ts", ".go"};
    }

    auto indexer = std::make_unique<CodeIndexer>(static_cast<std::size_t>(max_results));

    std::cout << "[INFO] Building code index for: " << codebase_path.string() << "\n";
    indexer->build(codebase_path, extensions);

    if (indexer->is_built()) {
        std::cout << "[INFO] Code index built: " << indexer->indexed_files()
                  << " files, " << indexer->index_size() << " unique tokens\n";
    } else {
        std::cerr << "[WARN] Code index build failed or path not found\n";
    }

    return indexer;
}

} // namespace logmonitor
