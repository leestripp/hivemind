// tests/test_secret_store.cpp — SecretStore broker (Phase 15), no LLM.
#include "hivemind/SecretStore.h"

#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sys/stat.h>

namespace fs = std::filesystem;
using namespace hivemind;

int main() {
    fs::path tmp = fs::temp_directory_path() / "hivemind_test_secret_store";
    fs::remove_all(tmp);
    fs::create_directories(tmp);

    SecretStore store{tmp / "secrets.db", tmp / "secrets.key"};

    // ── name validation ───────────────────────────────────────────────────
    assert(SecretStore::valid_name("OPENAI_KEY"));
    assert(SecretStore::valid_name("_private1"));
    assert(!SecretStore::valid_name(""));
    assert(!SecretStore::valid_name("1starts-digit"));
    assert(!SecretStore::valid_name("has-dash"));
    assert(!SecretStore::valid_name("has space"));
    assert(!SecretStore::valid_name(std::string(65, 'a')));

    // ── put / get roundtrip ───────────────────────────────────────────────
    assert(store.put("OPENAI_KEY", "sk-test-12345"));
    assert(!store.put("OPENAI_KEY", "other"));          // duplicate → false
    assert(!store.put("bad name", "x"));                // invalid name
    assert(!store.put("EMPTY", ""));                    // empty value
    assert(!store.put("MULTI", "line1\nline2"));       // newline values rejected
    auto v = store.get("OPENAI_KEY");
    assert(v && *v == "sk-test-12345");
    assert(!store.get("MISSING"));

    // ── master key file: created once, 0600, reused across instances ──────
    struct stat st{};
    assert(::stat((tmp / "secrets.key").c_str(), &st) == 0);
    assert((st.st_mode & 0777) == 0600);
    {
        SecretStore store2{tmp / "secrets.db", tmp / "secrets.key"};
        auto v2 = store2.get("OPENAI_KEY");
        assert(v2 && *v2 == "sk-test-12345");   // persistence + key reuse
    }

    // ── wrong key → GCM tag verification fails ────────────────────────────
    {
        fs::path tmp2 = fs::temp_directory_path() / "hivemind_test_secret_wrong";
        fs::remove_all(tmp2);
        fs::create_directories(tmp2);
        SecretStore rogue{tmp2 / "secrets.db", tmp2 / "secrets.key"};
        assert(rogue.put("SEED", "x"));   // generates a DIFFERENT key
        bool threw = false;
        try {
            SecretStore wrong{tmp / "secrets.db", tmp2 / "secrets.key"};
            (void)wrong.get("OPENAI_KEY");
        } catch (const std::exception&) { threw = true; }
        assert(threw);
    }

    // ── list never exposes values ─────────────────────────────────────────
    assert(store.put("AWS_SECRET", "aws-value-999"));
    auto entries = store.list();
    assert(entries.size() == 2);
    for (auto& e : entries) assert(!e.name.empty());

    // ── grants ────────────────────────────────────────────────────────────
    assert(store.grant("OPENAI_KEY", "agent-a"));
    assert(store.grant("OPENAI_KEY", "agent-b"));
    assert(store.grant("AWS_SECRET", "agent-b"));
    assert(!store.grant("MISSING", "agent-a"));   // unknown secret
    auto ga = store.grants_for_agent("agent-a");
    assert(ga.size() == 1 && ga[0] == "OPENAI_KEY");
    auto gb = store.grants_for_agent("agent-b");
    assert(gb.size() == 2);
    // list() joins grants
    for (auto& e : store.list())
        if (e.name == "OPENAI_KEY") assert(e.agent_ids.size() == 2);

    // ── env_for_agent: decrypted pairs, uppercased HIVE_SECRET_ prefix ────
    auto env_b = store.env_for_agent("agent-b");
    assert(env_b.size() == 2);
    bool saw_openai = false, saw_aws = false;
    for (auto& [k, val] : env_b) {
        if (k == "HIVE_SECRET_OPENAI_KEY") { saw_openai = true; assert(val == "sk-test-12345"); }
        if (k == "HIVE_SECRET_AWS_SECRET") { saw_aws = true; assert(val == "aws-value-999"); }
        assert(k.rfind("HIVE_SECRET_", 0) == 0);
    }
    assert(saw_openai && saw_aws);
    assert(store.env_for_agent("agent-nobody").empty());

    // ── revoke + remove cascade ───────────────────────────────────────────
    assert(store.revoke("OPENAI_KEY", "agent-b"));
    assert(store.grants_for_agent("agent-b").size() == 1);
    assert(store.remove("OPENAI_KEY"));           // deletes grants too
    assert(!store.exists("OPENAI_KEY"));
    assert(store.grants_for_agent("agent-a").empty());
    assert(!store.remove("OPENAI_KEY"));          // already gone

    fs::remove_all(tmp);
    fs::remove_all(fs::temp_directory_path() / "hivemind_test_secret_wrong");
    std::cout << "test_secret_store OK\n";
    return 0;
}
