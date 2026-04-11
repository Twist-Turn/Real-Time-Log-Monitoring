#pragma once
/**
 * @file CodeIndexer.hpp
 * @brief RAG (Retrieval-Augmented Generation) engine for mapping error
 *        messages to source code locations.
 *
 * At startup, recursively indexes a codebase directory, extracting:
 *   - Function/method names (regex-based per language)
 *   - Class names
 *   - File names
 *   - String literals (potential error messages)
 *
 * Builds a TF-IDF inverted index. When an error is detected, tokens are
 * extracted from the error line and scored against the index to find the
 * most relevant source code locations.
 *
 * Thread-safe: the index is immutable after build(). Query is read-only.
 */

#include "PatternEngine.hpp"  // for CodeLocation

#include <nlohmann/json_fwd.hpp>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace logmonitor {

/// A single entry in the inverted index
struct IndexEntry {
    std::string file_path;
    int line_number;
    std::string context;     // The line of code containing the token
    std::string function;    // Enclosing function name if known
};

class CodeIndexer {
public:
    explicit CodeIndexer(std::size_t max_results = 3)
        : max_results_(max_results) {}

    /**
     * @brief Build the index by scanning a directory recursively.
     * @param root       Path to the codebase root
     * @param extensions File extensions to index (e.g., {".cpp", ".py"})
     */
    void build(const std::filesystem::path& root,
               const std::vector<std::string>& extensions) {
        if (!std::filesystem::exists(root)) return;

        std::unordered_set<std::string> ext_set(extensions.begin(), extensions.end());
        total_files_ = 0;

        for (const auto& entry : std::filesystem::recursive_directory_iterator(
                 root, std::filesystem::directory_options::skip_permission_denied)) {
            if (!entry.is_regular_file()) continue;

            auto ext = entry.path().extension().string();
            if (ext_set.find(ext) == ext_set.end()) continue;

            index_file(entry.path());
            ++total_files_;
        }

        compute_idf();
        built_ = true;
    }

    /**
     * @brief Query the index with an error message.
     * @param error_line The log line that triggered the alert
     * @return Top-N most relevant code locations
     */
    [[nodiscard]] std::vector<CodeLocation> query(std::string_view error_line) const {
        if (!built_ || total_files_ == 0) return {};

        auto tokens = extract_error_tokens(error_line);
        if (tokens.empty()) return {};

        // Score each file by summing TF-IDF for matched tokens
        std::unordered_map<std::string, float> file_scores;
        std::unordered_map<std::string, const IndexEntry*> best_entry_per_file;

        for (const auto& token : tokens) {
            auto it = inverted_index_.find(token);
            if (it == inverted_index_.end()) continue;

            float idf = get_idf(token);

            for (const auto& entry : it->second) {
                // TF = 1.0 for simplicity (presence-based)
                float score = 1.0f * idf;

                // Boost: if the token is a filename that matches the entry's file
                if (entry.file_path.find(token) != std::string::npos) {
                    score *= 2.0f;
                }

                file_scores[entry.file_path] += score;

                // Track the best entry (highest scoring line) per file
                auto& best = best_entry_per_file[entry.file_path];
                if (!best || score > 0) {
                    best = &entry;
                }
            }
        }

        // Sort by score descending
        std::vector<std::pair<std::string, float>> ranked(
            file_scores.begin(), file_scores.end());
        std::sort(ranked.begin(), ranked.end(),
                  [](const auto& a, const auto& b) { return a.second > b.second; });

        // Build results
        std::vector<CodeLocation> results;
        std::size_t count = std::min(max_results_, ranked.size());
        for (std::size_t i = 0; i < count; ++i) {
            const auto& [file, score] = ranked[i];
            const auto* entry = best_entry_per_file[file];

            CodeLocation loc;
            loc.file_path = file;
            loc.line_number = entry ? entry->line_number : 0;
            loc.function_name = entry ? entry->function : "";
            loc.snippet = entry ? entry->context : "";
            loc.relevance_score = score;
            results.push_back(std::move(loc));
        }

        return results;
    }

    [[nodiscard]] bool is_built() const { return built_; }
    [[nodiscard]] std::size_t indexed_files() const { return total_files_; }
    [[nodiscard]] std::size_t index_size() const { return inverted_index_.size(); }

private:
    /// Index a single source file
    void index_file(const std::filesystem::path& filepath) {
        std::ifstream file(filepath);
        if (!file.is_open()) return;

        std::string relative = filepath.string();
        std::string filename = filepath.filename().string();
        std::string current_function;
        int line_num = 0;

        // Index the filename itself as a token
        add_to_index(filename, {relative, 0, filename, ""});

        // Also index the stem (e.g., "UserService" from "UserService.java")
        std::string stem = filepath.stem().string();
        if (stem != filename) {
            add_to_index(stem, {relative, 0, filename, ""});
        }

        std::string line;
        while (std::getline(file, line)) {
            ++line_num;

            // Extract function/method names
            auto func = extract_function_name(line, filepath.extension().string());
            if (func) {
                current_function = *func;
                add_to_index(*func, {relative, line_num, line, *func});
            }

            // Extract class names
            auto cls = extract_class_name(line, filepath.extension().string());
            if (cls) {
                add_to_index(*cls, {relative, line_num, line, current_function});
            }

            // Extract string literals (potential error messages)
            extract_string_literals(line, relative, line_num, current_function);

            // Index significant identifiers (words 4+ chars)
            extract_identifiers(line, relative, line_num, current_function);
        }
    }

    void add_to_index(const std::string& token, IndexEntry entry) {
        if (token.empty() || token.size() < 3) return;

        // Track which files contain this token (for IDF)
        token_document_count_[token].insert(entry.file_path);

        inverted_index_[token].push_back(std::move(entry));
    }

    /// Extract function/method name from a line based on file extension
    static std::optional<std::string> extract_function_name(
            const std::string& line, const std::string& ext) {
        static const std::regex cpp_func(
            R"((?:[\w:]+\s+)+(\w+)\s*\([^)]*\)\s*\{?)");
        static const std::regex py_func(R"(def\s+(\w+)\s*\()");
        static const std::regex js_func(
            R"((?:function\s+(\w+)|(?:const|let|var)\s+(\w+)\s*=\s*(?:async\s*)?\())" );
        static const std::regex java_func(
            R"((?:public|private|protected|static|\s)+[\w<>\[\]]+\s+(\w+)\s*\()");
        static const std::regex go_func(R"(func\s+(?:\([^)]+\)\s+)?(\w+)\s*\()");

        std::smatch match;
        if (ext == ".cpp" || ext == ".hpp" || ext == ".cc" || ext == ".h") {
            if (std::regex_search(line, match, cpp_func)) return match[1].str();
        } else if (ext == ".py") {
            if (std::regex_search(line, match, py_func)) return match[1].str();
        } else if (ext == ".js" || ext == ".ts") {
            if (std::regex_search(line, match, js_func)) {
                return match[1].matched ? match[1].str() : match[2].str();
            }
        } else if (ext == ".java") {
            if (std::regex_search(line, match, java_func)) return match[1].str();
        } else if (ext == ".go") {
            if (std::regex_search(line, match, go_func)) return match[1].str();
        }
        return std::nullopt;
    }

    /// Extract class name from a line
    static std::optional<std::string> extract_class_name(
            const std::string& line, const std::string& /*ext*/) {
        static const std::regex class_pattern(
            R"((?:class|struct|interface)\s+(\w+))");
        std::smatch match;
        if (std::regex_search(line, match, class_pattern)) {
            return match[1].str();
        }
        return std::nullopt;
    }

    /// Extract string literals that could be error messages
    void extract_string_literals(const std::string& line,
                                  const std::string& file,
                                  int line_num,
                                  const std::string& func) {
        static const std::regex str_lit(R"("[^"]{5,60}")");
        auto begin = std::sregex_iterator(line.begin(), line.end(), str_lit);
        auto end = std::sregex_iterator();

        for (auto it = begin; it != end; ++it) {
            std::string literal = it->str();
            // Remove quotes
            literal = literal.substr(1, literal.size() - 2);
            add_to_index(literal, {file, line_num, line, func});
        }
    }

    /// Extract significant identifiers (4+ char words) from code
    void extract_identifiers(const std::string& line,
                              const std::string& file,
                              int line_num,
                              const std::string& func) {
        static const std::regex ident(R"(\b([A-Za-z_]\w{3,})\b)");
        auto begin = std::sregex_iterator(line.begin(), line.end(), ident);
        auto end = std::sregex_iterator();

        // Skip common language keywords
        static const std::unordered_set<std::string> stop_words = {
            "void", "int", "char", "bool", "auto", "const", "static",
            "return", "class", "struct", "public", "private", "protected",
            "virtual", "override", "include", "namespace", "using",
            "this", "self", "import", "from", "function", "true", "false",
            "null", "nullptr", "None", "undefined", "string", "else",
            "while", "break", "continue", "switch", "case", "default",
            "throw", "catch", "finally", "async", "await", "yield",
            "template", "typename"
        };

        for (auto it = begin; it != end; ++it) {
            std::string word = it->str();
            if (stop_words.count(word)) continue;
            add_to_index(word, {file, line_num, line, func});
        }
    }

    /// Compute IDF values after all files are indexed
    void compute_idf() {
        for (const auto& [token, files] : token_document_count_) {
            idf_[token] = std::log(
                static_cast<float>(total_files_) /
                static_cast<float>(files.size()));
        }
    }

    float get_idf(const std::string& token) const {
        auto it = idf_.find(token);
        return (it != idf_.end()) ? it->second : 0.0f;
    }

    /// Extract searchable tokens from an error log line
    static std::vector<std::string> extract_error_tokens(std::string_view line) {
        std::vector<std::string> tokens;

        // 1. Extract file paths/names (e.g., "UserService.java:42")
        static const std::regex file_ref(R"((\w+\.(?:cpp|hpp|py|java|js|ts|go|cc|h))\s*[:\(]?\s*(\d+)?)");
        std::string line_str(line);
        auto begin = std::sregex_iterator(line_str.begin(), line_str.end(), file_ref);
        auto end = std::sregex_iterator();
        for (auto it = begin; it != end; ++it) {
            tokens.push_back((*it)[1].str());  // filename
            // Also add the stem
            auto dot = (*it)[1].str().rfind('.');
            if (dot != std::string::npos) {
                tokens.push_back((*it)[1].str().substr(0, dot));
            }
        }

        // 2. Extract function-like references (e.g., "processPayment()")
        static const std::regex func_ref(R"((\w{3,})\s*\()");
        begin = std::sregex_iterator(line_str.begin(), line_str.end(), func_ref);
        for (auto it = begin; it != end; ++it) {
            tokens.push_back((*it)[1].str());
        }

        // 3. Extract CamelCase/PascalCase identifiers (likely class names)
        static const std::regex camel(R"(\b([A-Z][a-z]+(?:[A-Z][a-z]+)+)\b)");
        begin = std::sregex_iterator(line_str.begin(), line_str.end(), camel);
        for (auto it = begin; it != end; ++it) {
            tokens.push_back((*it)[1].str());
        }

        // 4. Extract snake_case identifiers (4+ chars)
        static const std::regex snake(R"(\b([a-z]\w*_\w+)\b)");
        begin = std::sregex_iterator(line_str.begin(), line_str.end(), snake);
        for (auto it = begin; it != end; ++it) {
            tokens.push_back((*it)[1].str());
        }

        // 5. Extract significant standalone words (5+ chars, not common log words)
        static const std::unordered_set<std::string> log_stop_words = {
            "ERROR", "error", "WARN", "warn", "INFO", "info",
            "CRITICAL", "critical", "DEBUG", "debug", "FATAL",
            "fatal", "Exception", "Traceback", "failed"
        };
        static const std::regex word(R"(\b([A-Za-z_]\w{4,})\b)");
        begin = std::sregex_iterator(line_str.begin(), line_str.end(), word);
        for (auto it = begin; it != end; ++it) {
            std::string w = (*it)[1].str();
            if (!log_stop_words.count(w)) {
                tokens.push_back(std::move(w));
            }
        }

        // Deduplicate
        std::sort(tokens.begin(), tokens.end());
        tokens.erase(std::unique(tokens.begin(), tokens.end()), tokens.end());

        return tokens;
    }

    // ─── Data ───
    std::size_t max_results_;
    bool built_{false};
    std::size_t total_files_{0};

    // Token → list of index entries
    std::unordered_map<std::string, std::vector<IndexEntry>> inverted_index_;

    // Token → set of file paths that contain it (for IDF computation)
    std::unordered_map<std::string, std::unordered_set<std::string>> token_document_count_;

    // Token → precomputed IDF score
    std::unordered_map<std::string, float> idf_;
};

/// Build CodeIndexer from config JSON (defined in CodeIndexer.cpp)
/// Forward-declared with nlohmann::json by value — requires <nlohmann/json.hpp> at call site.
std::unique_ptr<CodeIndexer> create_code_indexer_from_config(
    const nlohmann::json& config,
    const std::filesystem::path& config_dir);

} // namespace logmonitor
