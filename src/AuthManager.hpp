/**
 * @file AuthManager.hpp
 * @brief JWT HS256 token creation/validation, PBKDF2 password hashing,
 *        and cryptographic random token generation (OpenSSL).
 */
#pragma once

#include <nlohmann/json.hpp>

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace logmonitor {

// ─── JWT Claims ───────────────────────────────────────────────────────────────

struct JwtClaims {
    std::string sub;    // user_id
    std::string email;
    int64_t     iat{0}; // issued-at  (Unix seconds)
    int64_t     exp{0}; // expires-at (Unix seconds)
    std::string jti;    // token ID  (random_hex(16)) for revocation
};

// ─── AuthManager ─────────────────────────────────────────────────────────────

class AuthManager {
public:
    static constexpr int TOKEN_EXPIRY_SECONDS = 86400; // 24 h
    static constexpr int PBKDF2_ITERATIONS    = 100000;
    static constexpr int PBKDF2_KEY_LEN       = 32;   // 256-bit derived key
    static constexpr int SALT_BYTES           = 16;

    explicit AuthManager(std::string jwt_secret)
        : jwt_secret_(std::move(jwt_secret)) {}

    // ─── Password utilities (all static, no secret needed) ──────────────────

    /// Returns a hex-encoded 16-byte random salt.
    static std::string generate_salt() {
        return random_hex(static_cast<std::size_t>(SALT_BYTES));
    }

    /// PBKDF2-SHA256(password, salt, 100k, 32) → hex string.
    static std::string hash_password(const std::string& password,
                                     const std::string& salt_hex) {
        auto salt = hex_to_bytes(salt_hex);
        unsigned char out[PBKDF2_KEY_LEN];
        if (PKCS5_PBKDF2_HMAC(
                password.c_str(), static_cast<int>(password.size()),
                salt.data(),      static_cast<int>(salt.size()),
                PBKDF2_ITERATIONS, EVP_sha256(),
                PBKDF2_KEY_LEN, out) != 1) {
            throw std::runtime_error("PBKDF2 failed");
        }
        return bytes_to_hex(out, static_cast<std::size_t>(PBKDF2_KEY_LEN));
    }

    static bool verify_password(const std::string& password,
                                 const std::string& salt_hex,
                                 const std::string& stored_hash) {
        auto computed = hash_password(password, salt_hex);
        if (computed.size() != stored_hash.size()) return false;
        return CRYPTO_memcmp(computed.data(), stored_hash.data(),
                             computed.size()) == 0;
    }

    // ─── JWT ────────────────────────────────────────────────────────────────

    std::string create_token(const std::string& user_id,
                             const std::string& email) {
        using json = nlohmann::json;

        int64_t now = static_cast<int64_t>(
            std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::system_clock::now().time_since_epoch()).count());

        std::string header_b64 = base64url_encode_str(
            R"({"alg":"HS256","typ":"JWT"})");

        json payload;
        payload["sub"]   = user_id;
        payload["email"] = email;
        payload["iat"]   = now;
        payload["exp"]   = now + TOKEN_EXPIRY_SECONDS;
        payload["jti"]   = random_hex(16);

        std::string payload_b64 = base64url_encode_str(payload.dump());

        std::string signing_input = header_b64 + "." + payload_b64;
        std::string sig_b64 = base64url_encode(hmac_sha256(signing_input));

        return signing_input + "." + sig_b64;
    }

    bool verify_token(const std::string& token, JwtClaims& out_claims) {
        // Split into 3 parts
        auto p1 = token.find('.');
        if (p1 == std::string::npos) return false;
        auto p2 = token.find('.', p1 + 1);
        if (p2 == std::string::npos) return false;

        std::string header_b64  = token.substr(0, p1);
        std::string payload_b64 = token.substr(p1 + 1, p2 - p1 - 1);
        std::string sig_b64     = token.substr(p2 + 1);

        // Recompute signature
        std::string signing_input = header_b64 + "." + payload_b64;
        std::string expected_b64  = base64url_encode(hmac_sha256(signing_input));

        // Constant-time comparison
        if (expected_b64.size() != sig_b64.size()) return false;
        if (CRYPTO_memcmp(expected_b64.data(), sig_b64.data(),
                          expected_b64.size()) != 0) return false;

        // Decode payload
        std::string payload_json;
        try {
            auto bytes = base64url_decode(payload_b64);
            payload_json.assign(bytes.begin(), bytes.end());
        } catch (...) { return false; }

        nlohmann::json j;
        try { j = nlohmann::json::parse(payload_json); }
        catch (...) { return false; }

        out_claims.sub   = j.value("sub",   "");
        out_claims.email = j.value("email", "");
        out_claims.iat   = j.value("iat",   int64_t{0});
        out_claims.exp   = j.value("exp",   int64_t{0});
        out_claims.jti   = j.value("jti",   "");

        // Check expiry
        int64_t now = static_cast<int64_t>(
            std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::system_clock::now().time_since_epoch()).count());
        if (out_claims.exp <= now) return false;

        // Check revocation
        if (!out_claims.jti.empty() && is_revoked(out_claims.jti)) return false;

        return true;
    }

    // ─── Revocation ─────────────────────────────────────────────────────────

    void revoke_token(const std::string& jti, int64_t exp_unix_sec = 0) {
        if (jti.empty()) return;
        std::lock_guard<std::mutex> lk(revocation_mutex_);
        revoked_jtis_[jti] = exp_unix_sec;
    }

    bool is_revoked(const std::string& jti) {
        if (jti.empty()) return false;
        std::lock_guard<std::mutex> lk(revocation_mutex_);
        // Prune expired entries
        int64_t now = static_cast<int64_t>(
            std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::system_clock::now().time_since_epoch()).count());
        for (auto it = revoked_jtis_.begin(); it != revoked_jtis_.end();) {
            if (it->second > 0 && it->second < now)
                it = revoked_jtis_.erase(it);
            else
                ++it;
        }
        return revoked_jtis_.count(jti) > 0;
    }

    // ─── Random hex token ───────────────────────────────────────────────────

    /// Generate `bytes` random bytes and return as lowercase hex string (2*bytes chars).
    static std::string random_hex(std::size_t bytes) {
        std::vector<unsigned char> buf(bytes);
        if (RAND_bytes(buf.data(), static_cast<int>(bytes)) != 1)
            throw std::runtime_error("RAND_bytes failed");
        return bytes_to_hex(buf.data(), bytes);
    }

private:
    // ─── Base64url (RFC 4648 §5, no padding) ────────────────────────────────

    static std::string base64url_encode(const std::string& data) {
        return base64url_encode(
            reinterpret_cast<const unsigned char*>(data.data()), data.size());
    }

    static std::string base64url_encode(const unsigned char* data,
                                        std::size_t len) {
        static const char* kChars =
            "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
        std::string out;
        out.reserve((len + 2) / 3 * 4);
        for (std::size_t i = 0; i < len; i += 3) {
            uint32_t b = static_cast<uint32_t>(data[i]) << 16;
            if (i + 1 < len) b |= static_cast<uint32_t>(data[i + 1]) << 8;
            if (i + 2 < len) b |= static_cast<uint32_t>(data[i + 2]);
            out += kChars[(b >> 18) & 0x3F];
            out += kChars[(b >> 12) & 0x3F];
            if (i + 1 < len) out += kChars[(b >> 6) & 0x3F];
            if (i + 2 < len) out += kChars[b & 0x3F];
        }
        return out;
    }

    static std::string base64url_encode_str(const std::string& s) {
        return base64url_encode(
            reinterpret_cast<const unsigned char*>(s.data()), s.size());
    }

    static std::vector<unsigned char> base64url_decode(const std::string& input) {
        auto val = [](char c) -> int {
            if (c >= 'A' && c <= 'Z') return c - 'A';
            if (c >= 'a' && c <= 'z') return c - 'a' + 26;
            if (c >= '0' && c <= '9') return c - '0' + 52;
            if (c == '-' || c == '+') return 62;
            if (c == '_' || c == '/') return 63;
            return -1;
        };
        std::vector<unsigned char> out;
        out.reserve(input.size() * 3 / 4);
        int buf = 0, bits = 0;
        for (char c : input) {
            int v = val(c);
            if (v < 0) continue;
            buf = (buf << 6) | v;
            bits += 6;
            if (bits >= 8) {
                bits -= 8;
                out.push_back(static_cast<unsigned char>((buf >> bits) & 0xFF));
            }
        }
        return out;
    }

    // ─── HMAC-SHA256 ────────────────────────────────────────────────────────

    std::string hmac_sha256(const std::string& data) const {
        unsigned char digest[EVP_MAX_MD_SIZE];
        unsigned int  digest_len = 0;
        HMAC(EVP_sha256(),
             jwt_secret_.data(),    static_cast<int>(jwt_secret_.size()),
             reinterpret_cast<const unsigned char*>(data.data()),
             static_cast<int>(data.size()),
             digest, &digest_len);
        return std::string(reinterpret_cast<char*>(digest),
                           static_cast<std::size_t>(digest_len));
    }

    // ─── Hex utilities ──────────────────────────────────────────────────────

    static std::string bytes_to_hex(const unsigned char* data, std::size_t len) {
        static const char kHex[] = "0123456789abcdef";
        std::string out(len * 2, '\0');
        for (std::size_t i = 0; i < len; ++i) {
            out[i * 2]     = kHex[(data[i] >> 4) & 0xF];
            out[i * 2 + 1] = kHex[data[i] & 0xF];
        }
        return out;
    }

    static std::vector<unsigned char> hex_to_bytes(const std::string& hex) {
        std::vector<unsigned char> out(hex.size() / 2);
        for (std::size_t i = 0; i < out.size(); ++i) {
            auto h = [](char c) -> unsigned char {
                if (c >= '0' && c <= '9') return static_cast<unsigned char>(c - '0');
                if (c >= 'a' && c <= 'f') return static_cast<unsigned char>(c - 'a' + 10);
                if (c >= 'A' && c <= 'F') return static_cast<unsigned char>(c - 'A' + 10);
                return 0;
            };
            out[i] = static_cast<unsigned char>((h(hex[i*2]) << 4) | h(hex[i*2+1]));
        }
        return out;
    }

    // ─── Members ────────────────────────────────────────────────────────────
    std::string jwt_secret_;
    mutable std::mutex revocation_mutex_;
    std::unordered_map<std::string, int64_t> revoked_jtis_; // jti → exp (Unix sec)
};

} // namespace logmonitor
