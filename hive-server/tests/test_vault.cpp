// tests/test_vault.cpp — Phase 22: external vault adapter. Stub CLIs via
// HIVEMIND_PASS_BIN / HIVEMIND_OP_BIN; no real pass/op needed.
#include "hivemind/Vault.h"
#include "hivemind/SecretStore.h"

#include <cassert>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>

namespace fs = std::filesystem;

int main() {
    fs::path tmp = fs::temp_directory_path() / "hivemind_test_vault";
    fs::remove_all(tmp);
    fs::create_directories(tmp);

    // Stub pass: prints a two-line record (line 1 = the secret).
    {
        std::ofstream f(tmp / "pass-stub");
        f << "#!/bin/sh\nprintf 'stub-secret-123\\nuser: bob\\n'\n";
    }
    fs::permissions(tmp / "pass-stub", fs::perms::owner_all);
    // Stub op: echoes a token.
    {
        std::ofstream f(tmp / "op-stub");
        f << "#!/bin/sh\nprintf 'op-token-xyz\\n'\n";
    }
    fs::permissions(tmp / "op-stub", fs::perms::owner_all);
    setenv("HIVEMIND_PASS_BIN", (tmp / "pass-stub").c_str(), 1);
    setenv("HIVEMIND_OP_BIN", (tmp / "op-stub").c_str(), 1);

    // ── validation ────────────────────────────────────────────────────
    using hm::vault::validate_source;
    assert(validate_source("pass:work/github-token").empty());
    assert(validate_source("op://Private/GitHub/token").empty());
    assert(!validate_source("").empty());
    assert(!validate_source("pass:").empty());
    assert(!validate_source("pass:../etc").empty());
    assert(!validate_source("pass:a b").empty());          // space rejected
    assert(!validate_source("pass:a'$(id)'").empty());     // injection rejected
    assert(!validate_source("op://a/../b").empty());
    assert(!validate_source("file:///etc/passwd").empty());

    // ── fetch via stubs ───────────────────────────────────────────────
    auto v1 = hm::vault::fetch("pass:work/github-token");
    assert(v1 && *v1 == "stub-secret-123");   // first line only
    auto v2 = hm::vault::fetch("op://Private/GitHub/token");
    assert(v2 && *v2 == "op-token-xyz");
    // unknown scheme / bad ref → nullopt (never runs anything)
    assert(!hm::vault::fetch("pass:bad;rm -rf /"));
    assert(!hm::vault::fetch("weird:thing"));
    // missing binary → nullopt, no crash
    setenv("HIVEMIND_PASS_BIN", "/nonexistent/pass", 1);
    assert(!hm::vault::fetch("pass:work/github-token"));
    setenv("HIVEMIND_PASS_BIN", (tmp / "pass-stub").c_str(), 1);

    // ── SecretStore integration: external rows fetch at get() time ────
    hivemind::SecretStore store{tmp / "secrets.db", tmp / "secrets.key"};
    assert(store.put_external("GH_TOKEN", "pass:work/github-token"));
    assert(!store.put_external("GH_TOKEN", "pass:work/other"));   // dup
    assert(!store.put_external("BAD", "pass:a b"));               // bad source
    assert(!store.put_external("evil name", "pass:x"));           // bad name
    auto got = store.get("GH_TOKEN");
    assert(got && *got == "stub-secret-123");
    // list shows the source, never a value
    auto entries = store.list();
    assert(entries.size() == 1 && entries[0].source == "pass:work/github-token");
    // local secrets still work alongside
    assert(store.put("LOCAL_KEY", "local-value"));
    auto lv = store.get("LOCAL_KEY");
    assert(lv && *lv == "local-value");
    assert(store.list().size() == 2);
    // reopen: external row persists with source intact
    {
        hivemind::SecretStore store2{tmp / "secrets.db", tmp / "secrets.key"};
        auto e2 = store2.list();
        assert(e2.size() == 2);
        auto g2 = store2.get("GH_TOKEN");
        assert(g2 && *g2 == "stub-secret-123");
    }

    fs::remove_all(tmp);
    std::cout << "test_vault: OK\n";
    return 0;
}
