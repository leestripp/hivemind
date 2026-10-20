// tests/test_secret_delete.cpp — secret_delete_apply(): the op core
// behind the agent's secrets_delete tool (2026-10-20). Semantics: an
// agent's delete removes THEIR grant; the entry is destroyed only when
// the LAST holder calls it. No LLM required.
#include "hivemind/HubRouter.h"
#include "hivemind/SecretStore.h"

#include <algorithm>
#include <cassert>
#include <filesystem>
#include <iostream>

namespace fs = std::filesystem;
using nlohmann::json;

static bool granted(hivemind::SecretStore& s, const std::string& agent,
                    const std::string& name) {
    const auto g = s.grants_for_agent(agent);
    return std::find(g.begin(), g.end(), name) != g.end();
}

int main() {
    fs::path tmp = fs::temp_directory_path() / "hivemind_test_secret_delete";
    fs::remove_all(tmp);
    fs::create_directories(tmp);

    hivemind::SecretStore secrets{tmp / "secrets.db", tmp / "secrets.key"};
    const std::string alice = "agent-alice-1";
    const std::string bob   = "agent-bob-2";

    assert(secret_create_apply(secrets, "shared_key", "api_key",
                               {{"value", "sk-1"}}, alice)
               .value("ok", false));
    assert(secrets.grant("shared_key", bob));
    assert(secret_create_apply(secrets, "solo_key", "api_key",
                               {{"value", "sk-2"}}, alice)
               .value("ok", false));

    // Not granted (and blind: same error as "does not exist").
    {
        auto r = secret_delete_apply(secrets, "shared_key", "agent-nobody");
        assert(!r.value("ok", false));
        assert(secrets.exists("shared_key"));
        auto r2 = secret_delete_apply(secrets, "no_such_key", alice);
        assert(!r2.value("ok", false));
        // Blind: both errors take the same "not granted to you" shape
        // (the name is echoed, so full-string equality can't hold).
        assert(r.value("error", "").find("granted to you") !=
               std::string::npos);
        assert(r2.value("error", "").find("granted to you") !=
               std::string::npos);
    }

    // Shared secret: first holder's delete REVOKES only; entry survives,
    // the other holder keeps their grant. Result never names holders.
    {
        auto r = secret_delete_apply(secrets, "shared_key", alice);
        assert(r.value("ok", false));
        assert(r.value("result", json::object()).contains("revoked"));
        assert(secrets.exists("shared_key"));
        assert(!granted(secrets, alice, "shared_key"));
        assert(granted(secrets, bob, "shared_key"));
        assert(r.dump().find(bob) == std::string::npos);
        assert(secrets.get("shared_key").value_or("") == "sk-1");
    }

    // Now Bob is the LAST holder: his delete DESTROYS the entry.
    {
        auto r = secret_delete_apply(secrets, "shared_key", bob);
        assert(r.value("ok", false));
        assert(r.value("result", json::object()).contains("deleted"));
        assert(!secrets.exists("shared_key"));
        assert(!granted(secrets, bob, "shared_key"));
    }

    // Sole holder destroys immediately.
    {
        auto r = secret_delete_apply(secrets, "solo_key", alice);
        assert(r.value("ok", false));
        assert(r.value("result", json::object()).contains("deleted"));
        assert(!secrets.exists("solo_key"));
    }

    fs::remove_all(tmp);
    std::cout << "test_secret_delete: PASS\n";
    return 0;
}
