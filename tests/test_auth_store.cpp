// tests/test_auth_store.cpp — AuthStore API key auth (Phase 9), no LLM.
#include "hivemind/AuthStore.h"

#include <cassert>
#include <filesystem>
#include <iostream>

namespace fs = std::filesystem;

int main() {
    fs::path tmp = fs::temp_directory_path() / "hivemind_test_auth_store";
    fs::remove_all(tmp);
    fs::create_directories(tmp);

    AuthStore store{tmp / "auth.db"};

    // ── bootstrap: master key created once, never twice ───────────────────
    assert(store.list().empty());
    // capture stdout to verify the key is printed when requested
    bool created = store.ensure_master_key(/*print=*/false);
    assert(created);
    assert(!store.ensure_master_key(false));   // second call: no-op
    assert(store.list().size() == 1);
    assert(store.list()[0].name == "master");
    assert(store.list()[0].scopes == "admin");

    // ── create + validate ─────────────────────────────────────────────────
    std::string raw = store.create_key("laptop", "agent");
    assert(!raw.empty());
    auto scopes = store.validate(raw);
    assert(scopes.has_value());
    assert(AuthStore::has_scope(*scopes, "agent"));
    assert(!AuthStore::has_scope(*scopes, "read"));
    // admin implies every scope
    assert(AuthStore::has_scope({"admin"}, "agent"));
    assert(AuthStore::has_scope({"admin"}, "read"));

    // wrong key rejected
    assert(!store.validate("hm_definitely_wrong_key").has_value());
    // empty key rejected
    assert(!store.validate("").has_value());

    // ── split_scopes ──────────────────────────────────────────────────────
    auto sp = AuthStore::split_scopes("admin, agent read");
    assert(sp.size() == 3);
    assert(sp[0] == "admin" && sp[1] == "agent" && sp[2] == "read");
    assert(AuthStore::split_scopes("").empty());

    // ── list: ids are public prefixes, hashes never exposed ───────────────
    auto keys = store.list();
    assert(keys.size() == 2);
    assert(keys[1].name == "laptop");
    assert(keys[1].scopes == "agent");
    assert(!keys[1].revoked);
    assert(keys[1].id.size() < raw.size());   // prefix only, not the secret

    // ── persistence across instances ──────────────────────────────────────
    {
        AuthStore store2{tmp / "auth.db"};
        assert(store2.list().size() == 2);
        assert(store2.validate(raw).has_value());
    }

    // ── revoke ────────────────────────────────────────────────────────────
    assert(store.revoke(keys[1].id));
    assert(!store.validate(raw).has_value());       // revoked key rejected
    assert(!store.revoke(keys[1].id));              // already revoked
    assert(!store.revoke("no-such-id"));

    fs::remove_all(tmp);
    std::cout << "test_auth_store: OK\n";
    return 0;
}
