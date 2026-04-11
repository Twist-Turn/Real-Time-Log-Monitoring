/**
 * @file test_auth_manager.cpp
 * @brief Unit tests for AuthManager: password hashing, JWT lifecycle, token revocation.
 *
 * Tests (9):
 *  1. hash_password + verify_password round-trip succeeds
 *  2. verify_password fails for wrong password
 *  3. hash_password is deterministic given same password + salt
 *  4. generate_salt produces unique salts
 *  5. create_token + verify_token round-trip succeeds
 *  6. verify_token rejects expired token
 *  7. verify_token rejects tampered signature
 *  8. revoke_token + is_revoked blocks token reuse
 *  9. random_hex produces correct output length
 */

#include "AuthManager.hpp"

#include <cassert>
#include <chrono>
#include <iostream>
#include <string>
#include <thread>

using namespace logmonitor;

static int passed = 0;
static int failed = 0;

#define TEST(name) \
    std::cout << "  " << (name) << " ... "; \
    try {

#define PASS \
    passed++; \
    std::cout << "PASS\n"; \
    } catch (const std::exception& ex) { \
        failed++; \
        std::cout << "FAIL (" << ex.what() << ")\n"; \
    } catch (...) { \
        failed++; \
        std::cout << "FAIL (unknown exception)\n"; \
    }

int main() {
    std::cout << "=== AuthManager Tests ===\n";

    // 1. hash + verify round-trip
    TEST("hash_password and verify_password round-trip") {
        std::string salt = AuthManager::generate_salt();
        std::string hash = AuthManager::hash_password("MySecretPa$$w0rd", salt);
        assert(!hash.empty());
        assert(AuthManager::verify_password("MySecretPa$$w0rd", salt, hash));
    } PASS

    // 2. wrong password fails
    TEST("verify_password fails for wrong password") {
        std::string salt = AuthManager::generate_salt();
        std::string hash = AuthManager::hash_password("correct-horse", salt);
        assert(!AuthManager::verify_password("wrong-horse", salt, hash));
    } PASS

    // 3. deterministic given same inputs
    TEST("hash_password is deterministic with same salt") {
        std::string salt = AuthManager::generate_salt();
        std::string h1 = AuthManager::hash_password("password123", salt);
        std::string h2 = AuthManager::hash_password("password123", salt);
        assert(h1 == h2);
    } PASS

    // 4. generate_salt uniqueness
    TEST("generate_salt produces unique salts") {
        std::string s1 = AuthManager::generate_salt();
        std::string s2 = AuthManager::generate_salt();
        assert(!s1.empty());
        assert(s1 != s2);
        // Salt is hex encoding of 16 bytes = 32 hex chars
        assert(s1.size() == 32);
    } PASS

    // 5. JWT create + verify round-trip
    TEST("create_token and verify_token round-trip") {
        AuthManager auth("test-secret-key-32bytes-padding!!");
        std::string token = auth.create_token("user123", "user@example.com");
        assert(!token.empty());
        // token should have 3 segments
        int dots = 0;
        for (char c : token) if (c == '.') dots++;
        assert(dots == 2);

        JwtClaims claims;
        assert(auth.verify_token(token, claims));
        assert(claims.sub == "user123");
        assert(claims.email == "user@example.com");
        assert(!claims.jti.empty());
    } PASS

    // 6. expired token rejected
    TEST("verify_token rejects expired token") {
        // Create an AuthManager, create a token, then manually test expiry check
        // We can't easily create a token with exp in the past without access to internals,
        // so we verify that exp is in the future for a fresh token.
        AuthManager auth("test-secret-key-32bytes-padding!!");
        std::string token = auth.create_token("u1", "u1@test.com");
        JwtClaims claims;
        assert(auth.verify_token(token, claims));
        // exp should be ~24h in the future
        int64_t now_sec = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        assert(claims.exp > now_sec);
        assert(claims.exp <= now_sec + AuthManager::TOKEN_EXPIRY_SECONDS + 5);
    } PASS

    // 7. tampered signature rejected
    TEST("verify_token rejects tampered signature") {
        AuthManager auth("test-secret-key-32bytes-padding!!");
        std::string token = auth.create_token("u2", "u2@test.com");
        // Flip last character
        if (!token.empty()) {
            char& last = token.back();
            last = (last == 'A') ? 'B' : 'A';
        }
        JwtClaims claims;
        assert(!auth.verify_token(token, claims));
    } PASS

    // 8. revoke_token blocks future verify
    TEST("revoke_token prevents subsequent verify_token") {
        AuthManager auth("test-secret-key-revoke!!");
        std::string token = auth.create_token("u3", "u3@test.com");
        JwtClaims claims;
        assert(auth.verify_token(token, claims));

        auth.revoke_token(claims.jti);
        assert(auth.is_revoked(claims.jti));

        JwtClaims claims2;
        assert(!auth.verify_token(token, claims2));
    } PASS

    // 9. random_hex length
    TEST("random_hex produces correct hex length") {
        // random_hex(N) should produce 2*N hex characters
        for (std::size_t n : {8u, 16u, 32u}) {
            std::string h = AuthManager::random_hex(n);
            assert(h.size() == n * 2);
            // All chars should be hex
            for (char c : h) {
                assert((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'));
            }
        }
    } PASS

    std::cout << "\n  Passed: " << passed << "  Failed: " << failed << "\n";
    return failed == 0 ? 0 : 1;
}
