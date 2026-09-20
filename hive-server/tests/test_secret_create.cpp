// tests/test_secret_create.cpp — secret_create_apply(): the op core
// behind the agent's secrets_create tool. No LLM required.
#include "hivemind/HubRouter.h"
#include "hivemind/SecretStore.h"

#include <algorithm>
#include <cassert>
#include <filesystem>
#include <iostream>

namespace fs = std::filesystem;
using nlohmann::json;

int main() {
    fs::path tmp = fs::temp_directory_path() / "hivemind_test_secret_create";
    fs::remove_all(tmp);
    fs::create_directories(tmp);

    hivemind::SecretStore secrets{tmp / "secrets.db", tmp / "secrets.key"};
    const std::string creator = "agent-creator-1";
    const std::string other   = "agent-other-9";

    // api_key happy path: created, granted to creator only.
    {
        auto r = secret_create_apply(secrets, "myservice_key", "api_key",
                                     {{"value", "sk-live-123"}}, creator);
        assert(r.value("ok", false));
        assert(secrets.exists("myservice_key"));
        auto cg = secrets.grants_for_agent(creator);
        auto og = secrets.grants_for_agent(other);
        assert(std::find(cg.begin(), cg.end(), "myservice_key") != cg.end());
        assert(std::find(og.begin(), og.end(), "myservice_key") == og.end());
        // Result never carries the value.
        assert(r.dump().find("sk-live-123") == std::string::npos);
    }

    // login with full metadata.
    {
        auto r = secret_create_apply(secrets, "shop_login", "login",
                                     {{"username","hive-bot"},
                                      {"password","p@ss"},
                                      {"url","https://shop.example"}},
                                     creator);
        assert(r.value("ok", false));
    }

    // note type.
    {
        auto r = secret_create_apply(secrets, "deploy_note", "note",
                                     {{"text","multi\nline"}}, creator);
        assert(r.value("ok", false));
    }

    // duplicate name rejected (no clobber, even by the creator).
    {
        auto r = secret_create_apply(secrets, "myservice_key", "api_key",
                                     {{"value", "rotated"}}, creator);
        assert(!r.value("ok", false));
        assert(r.value("error", "").find("name taken") != std::string::npos);
        // Value untouched.
        assert(secrets.get("myservice_key").value_or("") == "sk-live-123");
    }

    // validation: bad name, bad type, missing material per type.
    {
        assert(!secret_create_apply(secrets, "bad name!", "api_key",
                                    {{"value","x"}}, creator)
                    .value("ok", false));
        assert(!secret_create_apply(secrets, "okname", "weird",
                                    {{"value","x"}}, creator)
                    .value("ok", false));
        assert(!secret_create_apply(secrets, "login2", "login",
                                    {{"username","u"}}, creator)
                    .value("ok", false));   // no password
        assert(!secret_create_apply(secrets, "note2", "note",
                                    json::object(), creator)
                    .value("ok", false));   // no text
        assert(!secret_create_apply(secrets, "key2", "api_key",
                                    json::object(), creator)
                    .value("ok", false));   // no value
    }

    fs::remove_all(tmp);
    std::cout << "test_secret_create: OK\n";
    return 0;
}
