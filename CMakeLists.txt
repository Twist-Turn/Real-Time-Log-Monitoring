cmake_minimum_required(VERSION 3.14)
project(LogMonitor VERSION 1.0.0 LANGUAGES CXX)

# ─── C++17 Strict ───
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ─── Compiler warnings (interview-ready quality) ───
add_compile_options(
    -Wall -Wextra -Wpedantic -Wshadow -Wconversion
    -Wnon-virtual-dtor -Wold-style-cast -Wcast-align
    -Wunused -Woverloaded-virtual -Wformat=2
)

# ─── Fetch nlohmann/json ───
include(FetchContent)
FetchContent_Declare(
    nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG        v3.11.3
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(nlohmann_json)

# ─── Fetch cpp-httplib ───
FetchContent_Declare(
    cpp_httplib
    GIT_REPOSITORY https://github.com/yhirose/cpp-httplib.git
    GIT_TAG        v0.15.3
    GIT_SHALLOW    TRUE
)
set(HTTPLIB_USE_OPENSSL_IF_AVAILABLE OFF CACHE BOOL "" FORCE)
set(HTTPLIB_USE_ZLIB_IF_AVAILABLE    OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(cpp_httplib)

# ─── Find ncurses ───
find_package(Curses REQUIRED)

# ─── Find pthreads ───
set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)

# ─── Find libcurl (Phase 4: webhook delivery) ───
find_package(CURL REQUIRED)

# ─── Find OpenSSL (Auth: JWT HS256, PBKDF2 password hashing) ───
find_package(OpenSSL REQUIRED)

# ─── Main executable ───
add_executable(logmonitor
    src/main.cpp
    src/PatternEngine.cpp
    src/CodeIndexer.cpp
)

target_include_directories(logmonitor PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CURSES_INCLUDE_DIR}
)

target_link_libraries(logmonitor PRIVATE
    nlohmann_json::nlohmann_json
    httplib::httplib
    CURL::libcurl
    OpenSSL::Crypto
    ${CURSES_LIBRARIES}
    Threads::Threads
)

# Platform-specific: link network libraries if needed
if(WIN32)
    target_link_libraries(logmonitor PRIVATE ws2_32)
endif()

# ─── Tests ───
enable_testing()

# Test: Ring Buffer
add_executable(test_ring_buffer tests/test_ring_buffer.cpp)
target_include_directories(test_ring_buffer PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(test_ring_buffer PRIVATE Threads::Threads)
add_test(NAME RingBufferTest COMMAND test_ring_buffer)

# Test: Pattern Engine
add_executable(test_pattern_engine
    tests/test_pattern_engine.cpp
    src/PatternEngine.cpp
)
target_include_directories(test_pattern_engine PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(test_pattern_engine PRIVATE
    nlohmann_json::nlohmann_json
    Threads::Threads
)
add_test(NAME PatternEngineTest COMMAND test_pattern_engine)

# Test: Pipeline (integration)
add_executable(test_pipeline
    tests/test_pipeline.cpp
    src/PatternEngine.cpp
    src/CodeIndexer.cpp
)
target_include_directories(test_pipeline PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CURSES_INCLUDE_DIR}
)
target_link_libraries(test_pipeline PRIVATE
    nlohmann_json::nlohmann_json
    ${CURSES_LIBRARIES}
    Threads::Threads
)
add_test(NAME PipelineTest COMMAND test_pipeline)

# Test: Code Indexer (RAG)
add_executable(test_code_indexer
    tests/test_code_indexer.cpp
    src/CodeIndexer.cpp
)
target_include_directories(test_code_indexer PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(test_code_indexer PRIVATE
    nlohmann_json::nlohmann_json
    Threads::Threads
)
add_test(NAME CodeIndexerTest COMMAND test_code_indexer)

# Test: Network Ingester
add_executable(test_network_ingester
    tests/test_network_ingester.cpp
    src/PatternEngine.cpp
    src/CodeIndexer.cpp
)
target_include_directories(test_network_ingester PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(test_network_ingester PRIVATE
    nlohmann_json::nlohmann_json
    Threads::Threads
)
add_test(NAME NetworkIngesterTest COMMAND test_network_ingester)

# Test: HTTP Server (Phase 1)
add_executable(test_http_server
    tests/test_http_server.cpp
    src/PatternEngine.cpp
)
target_include_directories(test_http_server PRIVATE
    ${CMAKE_SOURCE_DIR}/src
)
target_link_libraries(test_http_server PRIVATE
    nlohmann_json::nlohmann_json
    httplib::httplib
    OpenSSL::Crypto
    Threads::Threads
)
add_test(NAME HttpServerTest COMMAND test_http_server)

# Test: TSDB (Phase 2)
add_executable(test_tsdb
    tests/test_tsdb.cpp
)
target_include_directories(test_tsdb PRIVATE
    ${CMAKE_SOURCE_DIR}/src
)
target_link_libraries(test_tsdb PRIVATE
    nlohmann_json::nlohmann_json
    Threads::Threads
)
add_test(NAME TSDBTest COMMAND test_tsdb)

# Test: Alert Rules Engine (Phase 3)
add_executable(test_alert_rules
    tests/test_alert_rules.cpp
)
target_include_directories(test_alert_rules PRIVATE
    ${CMAKE_SOURCE_DIR}/src
)
target_link_libraries(test_alert_rules PRIVATE
    nlohmann_json::nlohmann_json
    OpenSSL::Crypto
    Threads::Threads
)
add_test(NAME AlertRulesTest COMMAND test_alert_rules)

# Test: AuthManager (SaaS Phase A)
add_executable(test_auth_manager
    tests/test_auth_manager.cpp
)
target_include_directories(test_auth_manager PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(test_auth_manager PRIVATE
    nlohmann_json::nlohmann_json
    OpenSSL::Crypto
    Threads::Threads
)
add_test(NAME AuthManagerTest COMMAND test_auth_manager)

# Test: UserStore + ProjectStore (SaaS Phase B)
add_executable(test_stores
    tests/test_stores.cpp
)
target_include_directories(test_stores PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(test_stores PRIVATE
    nlohmann_json::nlohmann_json
    OpenSSL::Crypto
    Threads::Threads
)
add_test(NAME StoresTest COMMAND test_stores)

# Test: TenantManager (SaaS Phase C)
add_executable(test_tenant_manager
    tests/test_tenant_manager.cpp
)
target_include_directories(test_tenant_manager PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(test_tenant_manager PRIVATE
    nlohmann_json::nlohmann_json
    OpenSSL::Crypto
    Threads::Threads
)
add_test(NAME TenantManagerTest COMMAND test_tenant_manager)

# Test: HTTP multi-tenant (SaaS Phase D, port 19091)
add_executable(test_http_multitenant
    tests/test_http_multitenant.cpp
    src/PatternEngine.cpp
)
target_include_directories(test_http_multitenant PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(test_http_multitenant PRIVATE
    nlohmann_json::nlohmann_json
    httplib::httplib
    OpenSSL::Crypto
    Threads::Threads
)
add_test(NAME HttpMultitenantTest COMMAND test_http_multitenant)

# ─── Install ───
install(TARGETS logmonitor DESTINATION bin)
install(FILES config/config.json DESTINATION etc/logmonitor)
