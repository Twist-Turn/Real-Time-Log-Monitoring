/**
 * @file test_stores.cpp
 * @brief Unit tests for UserStore and ProjectStore.
 *
 * Tests (12):
 *  UserStore:
 *   1. register_user succeeds
 *   2. duplicate email returns error
 *   3. find_by_id returns correct user
 *   4. find_by_email for unknown email returns false
 *   5. persistence round-trip (save + reload)
 *
 *  ProjectStore:
 *   6. create_project succeeds, api_key has correct prefix
 *   7. find_by_api_key (O(1) index) works
 *   8. list_by_owner returns only that owner's projects
 *   9. delete_project removes project
 *  10. delete_project by wrong owner returns error
 *  11. rotate_api_key changes the key and old key is gone
 *  12. ProjectStore persistence round-trip
 */

#include "UserStore.hpp"
#include "ProjectStore.hpp"

#include <cassert>
#include <filesystem>
#include <iostream>
#include <string>

using namespace logmonitor;
namespace fs = std::filesystem;

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

static const std::string USERS_PATH  = "/tmp/test_stores_users.json";
static const std::string PROJ_PATH   = "/tmp/test_stores_projects.json";

static void cleanup() {
    fs::remove(USERS_PATH);
    fs::remove(PROJ_PATH);
    fs::remove(USERS_PATH + ".tmp");
    fs::remove(PROJ_PATH  + ".tmp");
}

int main() {
    std::cout << "=== UserStore + ProjectStore Tests ===\n";
    cleanup();

    // ── UserStore ────────────────────────────────────────────────────────────

    // 1. register succeeds
    TEST("UserStore: register_user succeeds") {
        fs::remove(USERS_PATH);
        UserStore store(USERS_PATH);
        User out;
        std::string err = store.register_user("alice@example.com", "password123", out);
        assert(err.empty());
        assert(!out.id.empty());
        assert(out.email == "alice@example.com");
        assert(!out.password_hash.empty());
        assert(!out.salt.empty());
        assert(out.created_at_ns > 0);
        assert(store.size() == 1);
    } PASS

    // 2. duplicate email
    TEST("UserStore: duplicate email returns error") {
        fs::remove(USERS_PATH);
        UserStore store(USERS_PATH);
        User out;
        store.register_user("dup@example.com", "password123", out);
        User out2;
        std::string err = store.register_user("dup@example.com", "different123", out2);
        assert(!err.empty());
    } PASS

    // 3. find_by_id
    TEST("UserStore: find_by_id returns correct user") {
        fs::remove(USERS_PATH);
        UserStore store(USERS_PATH);
        User created;
        store.register_user("bob@example.com", "password456", created);

        User found;
        assert(store.find_by_id(created.id, found));
        assert(found.id == created.id);
        assert(found.email == "bob@example.com");
    } PASS

    // 4. find_by_email unknown
    TEST("UserStore: find_by_email for unknown email returns false") {
        fs::remove(USERS_PATH);
        UserStore store(USERS_PATH);
        User out;
        assert(!store.find_by_email("nobody@example.com", out));
    } PASS

    // 5. persistence round-trip
    TEST("UserStore: persistence round-trip") {
        fs::remove(USERS_PATH);
        {
            UserStore store(USERS_PATH);
            User out;
            store.register_user("persist@example.com", "password789", out);
        }
        // Reload from disk
        UserStore store2(USERS_PATH);
        User found;
        assert(store2.find_by_email("persist@example.com", found));
        assert(found.email == "persist@example.com");
        assert(!found.password_hash.empty());
    } PASS

    // ── ProjectStore ─────────────────────────────────────────────────────────

    // 6. create project
    TEST("ProjectStore: create_project sets correct prefix on api_key") {
        fs::remove(PROJ_PATH);
        ProjectStore store(PROJ_PATH);
        Project out;
        std::string err = store.create_project("My App", "owner_001", out);
        assert(err.empty());
        assert(!out.id.empty());
        assert(out.name == "My App");
        assert(out.owner_id == "owner_001");
        assert(out.api_key.substr(0, 8) == "lm_proj_");
        assert(out.api_key.size() == 8 + 32);  // "lm_proj_" + 32 hex chars
        assert(out.created_at_ns > 0);
    } PASS

    // 7. find_by_api_key
    TEST("ProjectStore: find_by_api_key O(1) lookup works") {
        fs::remove(PROJ_PATH);
        ProjectStore store(PROJ_PATH);
        Project created;
        store.create_project("My App", "owner_001", created);

        Project found;
        assert(store.find_by_api_key(created.api_key, found));
        assert(found.id == created.id);
        assert(!store.find_by_api_key("lm_proj_badkey000000000000000000000000", found));
    } PASS

    // 8. list_by_owner
    TEST("ProjectStore: list_by_owner returns only that owner's projects") {
        fs::remove(PROJ_PATH);
        ProjectStore store(PROJ_PATH);
        Project p1, p2, p3;
        store.create_project("App A", "alice", p1);
        store.create_project("App B", "alice", p2);
        store.create_project("App C", "bob",   p3);

        auto alice_projects = store.list_by_owner("alice");
        assert(alice_projects.size() == 2);
        auto bob_projects = store.list_by_owner("bob");
        assert(bob_projects.size() == 1);
        assert(bob_projects[0].name == "App C");
    } PASS

    // 9. delete project
    TEST("ProjectStore: delete_project removes project") {
        fs::remove(PROJ_PATH);
        ProjectStore store(PROJ_PATH);
        Project created;
        store.create_project("ToDelete", "owner_del", created);

        std::string err = store.delete_project(created.id, "owner_del");
        assert(err.empty());

        Project found;
        assert(!store.find_by_id(created.id, found));
        assert(!store.find_by_api_key(created.api_key, found));
        assert(store.list_by_owner("owner_del").empty());
    } PASS

    // 10. delete wrong owner
    TEST("ProjectStore: delete_project by wrong owner returns error") {
        fs::remove(PROJ_PATH);
        ProjectStore store(PROJ_PATH);
        Project created;
        store.create_project("Protected", "real_owner", created);

        std::string err = store.delete_project(created.id, "other_owner");
        assert(!err.empty());

        // Project still exists
        Project found;
        assert(store.find_by_id(created.id, found));
    } PASS

    // 11. rotate_api_key
    TEST("ProjectStore: rotate_api_key changes key and invalidates old one") {
        fs::remove(PROJ_PATH);
        ProjectStore store(PROJ_PATH);
        Project created;
        store.create_project("Rotate Me", "owner_r", created);

        std::string old_key = created.api_key;
        std::string new_key = store.rotate_api_key(created.id, "owner_r");
        assert(!new_key.empty());
        assert(new_key != old_key);
        assert(new_key.substr(0, 8) == "lm_proj_");

        // Old key no longer valid
        Project found;
        assert(!store.find_by_api_key(old_key, found));
        // New key valid
        assert(store.find_by_api_key(new_key, found));
        assert(found.id == created.id);
    } PASS

    // 12. ProjectStore persistence
    TEST("ProjectStore: persistence round-trip") {
        fs::remove(PROJ_PATH);
        Project saved;
        {
            ProjectStore store(PROJ_PATH);
            store.create_project("Persisted App", "persisted_owner", saved);
        }
        ProjectStore store2(PROJ_PATH);
        Project found;
        assert(store2.find_by_id(saved.id, found));
        assert(found.name == "Persisted App");
        assert(found.api_key == saved.api_key);
    } PASS

    cleanup();
    std::cout << "\n  Passed: " << passed << "  Failed: " << failed << "\n";
    return failed == 0 ? 0 : 1;
}
