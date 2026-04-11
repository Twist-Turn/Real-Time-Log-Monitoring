/**
 * @file test_code_indexer.cpp
 * @brief Tests for the RAG CodeIndexer: indexing and TF-IDF querying.
 */

#include "CodeIndexer.hpp"

#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

using namespace logmonitor;

static const std::string TEST_DIR = "/tmp/test_code_indexer_src";

/// Create a temporary codebase for testing
void setup_test_codebase() {
    std::filesystem::create_directories(TEST_DIR);

    // C++ file
    {
        std::ofstream f(TEST_DIR + "/UserService.cpp");
        f << "#include <string>\n"
          << "\n"
          << "class UserService {\n"
          << "public:\n"
          << "    void processPayment(int userId) {\n"
          << "        if (userId <= 0) {\n"
          << "            throw std::runtime_error(\"Invalid user ID\");\n"
          << "        }\n"
          << "        // process payment logic\n"
          << "    }\n"
          << "\n"
          << "    std::string getUserName(int userId) {\n"
          << "        return \"user_\" + std::to_string(userId);\n"
          << "    }\n"
          << "};\n";
    }

    // Python file
    {
        std::ofstream f(TEST_DIR + "/auth_middleware.py");
        f << "import jwt\n"
          << "\n"
          << "class AuthMiddleware:\n"
          << "    def handle_request(self, request):\n"
          << "        token = request.headers.get('Authorization')\n"
          << "        if not token:\n"
          << "            raise ValueError(\"Token expired\")\n"
          << "        return self.validate_token(token)\n"
          << "\n"
          << "    def validate_token(self, token):\n"
          << "        return jwt.decode(token, 'secret', algorithms=['HS256'])\n";
    }

    // JavaScript file
    {
        std::ofstream f(TEST_DIR + "/server.js");
        f << "const express = require('express');\n"
          << "\n"
          << "function handleDatabaseError(err) {\n"
          << "    console.error('Database connection failed:', err);\n"
          << "    throw new Error('Database unavailable');\n"
          << "}\n"
          << "\n"
          << "const processOrder = async (orderId) => {\n"
          << "    const result = await db.query('SELECT * FROM orders WHERE id = ?', [orderId]);\n"
          << "    if (!result) throw new Error('Order not found');\n"
          << "    return result;\n"
          << "};\n";
    }
}

void cleanup_test_codebase() {
    std::filesystem::remove_all(TEST_DIR);
}

// ─── Test 1: Build index ───

void test_build_index() {
    CodeIndexer indexer(3);

    indexer.build(TEST_DIR, {".cpp", ".py", ".js"});

    assert(indexer.is_built());
    assert(indexer.indexed_files() == 3);
    assert(indexer.index_size() > 0);

    std::cout << "  [PASS] test_build_index (" << indexer.indexed_files()
              << " files, " << indexer.index_size() << " tokens)\n";
}

// ─── Test 2: Query matches C++ file ───

void test_query_cpp_error() {
    CodeIndexer indexer(3);
    indexer.build(TEST_DIR, {".cpp", ".py", ".js"});

    auto results = indexer.query(
        "NullPointerException at UserService.processPayment(UserService.cpp:5)");

    assert(!results.empty());

    // The top result should reference UserService.cpp
    bool found_user_service = false;
    for (const auto& loc : results) {
        if (loc.file_path.find("UserService.cpp") != std::string::npos) {
            found_user_service = true;
            assert(loc.relevance_score > 0.0f);
            break;
        }
    }
    assert(found_user_service);

    std::cout << "  [PASS] test_query_cpp_error (top: " << results[0].file_path
              << " score: " << results[0].relevance_score << ")\n";
}

// ─── Test 3: Query matches Python file ───

void test_query_python_error() {
    CodeIndexer indexer(3);
    indexer.build(TEST_DIR, {".cpp", ".py", ".js"});

    auto results = indexer.query(
        "ERROR [2026-03-27] auth_middleware.py:handle_request:4 - Token expired");

    assert(!results.empty());

    bool found_auth = false;
    for (const auto& loc : results) {
        if (loc.file_path.find("auth_middleware.py") != std::string::npos) {
            found_auth = true;
            break;
        }
    }
    assert(found_auth);

    std::cout << "  [PASS] test_query_python_error\n";
}

// ─── Test 4: Query matches JS file ───

void test_query_js_error() {
    CodeIndexer indexer(3);
    indexer.build(TEST_DIR, {".cpp", ".py", ".js"});

    auto results = indexer.query(
        "ERROR: handleDatabaseError - Database connection failed in server.js:4");

    assert(!results.empty());

    bool found_server = false;
    for (const auto& loc : results) {
        if (loc.file_path.find("server.js") != std::string::npos) {
            found_server = true;
            break;
        }
    }
    assert(found_server);

    std::cout << "  [PASS] test_query_js_error\n";
}

// ─── Test 5: Empty query ───

void test_empty_query() {
    CodeIndexer indexer(3);
    indexer.build(TEST_DIR, {".cpp", ".py", ".js"});

    auto results = indexer.query("");
    assert(results.empty());

    std::cout << "  [PASS] test_empty_query\n";
}

// ─── Test 6: Unbuilt indexer ───

void test_unbuilt_indexer() {
    CodeIndexer indexer(3);

    assert(!indexer.is_built());
    auto results = indexer.query("ERROR: something");
    assert(results.empty());

    std::cout << "  [PASS] test_unbuilt_indexer\n";
}

// ─── Test 7: Non-existent directory ───

void test_nonexistent_dir() {
    CodeIndexer indexer(3);
    indexer.build("/tmp/this_dir_does_not_exist_xyz", {".cpp"});

    assert(!indexer.is_built());

    std::cout << "  [PASS] test_nonexistent_dir\n";
}

// ─── Test 8: Max results limit ───

void test_max_results() {
    CodeIndexer indexer(1);  // max 1 result
    indexer.build(TEST_DIR, {".cpp", ".py", ".js"});

    auto results = indexer.query("ERROR UserService processPayment server handleDatabaseError");
    assert(results.size() <= 1);

    std::cout << "  [PASS] test_max_results\n";
}

int main() {
    std::cout << "=== Code Indexer (RAG) Tests ===\n";

    setup_test_codebase();

    test_build_index();
    test_query_cpp_error();
    test_query_python_error();
    test_query_js_error();
    test_empty_query();
    test_unbuilt_indexer();
    test_nonexistent_dir();
    test_max_results();

    cleanup_test_codebase();

    std::cout << "=== All Code Indexer Tests PASSED ===\n";
    return 0;
}
