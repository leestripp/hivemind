/**
 * @file test_secret_tools.cpp
 * @brief Unit tests for the secret-broker tools (secrets_list /
 *        secrets_get, Phase 61).
 *
 * The hub renders granted secrets as HIVE_SECRET_<NAME>[_USER|_PASS|_URL]
 * in the container environment (Phase 60). These tests set/unset those
 * variables directly and drive the tools' execute() with a stub context.
 */

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

#include "tools/SecretTools.h"

using json = nlohmann::json;

namespace {

class StubCtx final : public hive::HostToolContext {
public:
    std::string read_file(const std::filesystem::path&) override { return {}; }
    void write_file(const std::filesystem::path&,
                    const std::string&) override {}
    std::vector<std::string>
        list_directory(const std::filesystem::path&) override { return {}; }
    const std::filesystem::path& project_root() const override {
        static const std::filesystem::path root{"/tmp"};
        return root;
    }
};

// Unique prefix so parallel/leftover env from other tests can't collide.
void set_fixture_env() {
    ::setenv("HIVE_SECRET_ST61_TOKEN", "sk-test-123", 1);
    ::setenv("HIVE_SECRET_ST61_LOGIN_USER", "alice@example.com", 1);
    ::setenv("HIVE_SECRET_ST61_LOGIN_PASS", "hunter2", 1);
    ::setenv("HIVE_SECRET_ST61_LOGIN_URL", "https://example.com/login", 1);
}

void clear_fixture_env() {
    ::unsetenv("HIVE_SECRET_ST61_TOKEN");
    ::unsetenv("HIVE_SECRET_ST61_LOGIN_USER");
    ::unsetenv("HIVE_SECRET_ST61_LOGIN_PASS");
    ::unsetenv("HIVE_SECRET_ST61_LOGIN_URL");
}

}  // namespace

TEST_SUITE("SecretTools") {

TEST_CASE("name validation matches the hub convention") {
    CHECK(hive::secret_env_name_valid("GITHUB_TOKEN"));
    CHECK(hive::secret_env_name_valid("A1_"));
    CHECK_FALSE(hive::secret_env_name_valid(""));
    CHECK_FALSE(hive::secret_env_name_valid("1ABC"));       // leading digit
    CHECK_FALSE(hive::secret_env_name_valid("lower_case")); // lowercase
    CHECK_FALSE(hive::secret_env_name_valid("HAS-DASH"));
    CHECK_FALSE(hive::secret_env_name_valid(std::string(65, 'A')));
}

TEST_CASE("normalization uppercases") {
    CHECK(hive::secret_env_normalize("github_token") == "GITHUB_TOKEN");
    CHECK(hive::secret_env_normalize("Mixed_Case9") == "MIXED_CASE9");
}

TEST_CASE("schemas are well-formed objects (Phase 60c brace guard)") {
    hive::SecretsListTool list_tool;
    hive::SecretsGetTool  get_tool;
    for (const auto* t : {static_cast<hive::BaseTool*>(&list_tool),
                          static_cast<hive::BaseTool*>(&get_tool)}) {
        const json def = t->get_definition();
        REQUIRE(def.contains("function"));
        const auto& fn = def["function"];
        CHECK(fn["name"].get<std::string>() == t->get_name());
        REQUIRE(fn["parameters"].is_object());
        CHECK(fn["parameters"]["type"] == "object");
        CHECK(fn["parameters"]["properties"].is_object());
    }
    // secrets_get requires "name" and offers the field enum.
    const json get_def = get_tool.get_definition();
    const auto& gp = get_def["function"]["parameters"];
    CHECK(gp["required"] == json::array({"name"}));
    CHECK(gp["properties"]["field"]["enum"] ==
          json::array({"value", "username", "password", "url"}));
}

TEST_CASE("secrets_list shows grants without values") {
    set_fixture_env();
    hive::SecretsListTool tool;
    StubCtx ctx;
    const json res = tool.execute(json::object(), ctx);
    clear_fixture_env();

    REQUIRE(res.value("status", "") == "ok");
    const auto& secrets = res["secrets"];
    REQUIRE(secrets.is_array());

    bool found_token = false, found_login = false;
    for (const auto& s : secrets) {
        const auto name = s.value("name", "");
        if (name == "ST61_TOKEN") {
            found_token = true;
            CHECK(s.value("kind", "") == "value");
            CHECK(s["fields"] == json::array({"value"}));
            CHECK_FALSE(s.contains("username"));
        } else if (name == "ST61_LOGIN") {
            found_login = true;
            CHECK(s.value("kind", "") == "login");
            CHECK(s["fields"] ==
                  json::array({"username", "password", "url"}));
            // Metadata is shown; the password value is NOT.
            CHECK(s.value("username", "") == "alice@example.com");
            CHECK(s.value("url", "") == "https://example.com/login");
            CHECK(s.dump().find("hunter2") == std::string::npos);
        }
    }
    CHECK(found_token);
    CHECK(found_login);
}

TEST_CASE("secrets_get reads a granted value, any case") {
    set_fixture_env();
    hive::SecretsGetTool tool;
    StubCtx ctx;

    json res = tool.execute({{"name", "st61_token"}}, ctx);
    CHECK(res.value("status", "") == "ok");
    CHECK(res.value("name", "") == "ST61_TOKEN");
    CHECK(res.value("field", "") == "value");
    CHECK(res.value("value", "") == "sk-test-123");

    // Explicit "value" field behaves like omission.
    res = tool.execute({{"name", "ST61_TOKEN"}, {"field", "value"}}, ctx);
    CHECK(res.value("value", "") == "sk-test-123");
    clear_fixture_env();
}

TEST_CASE("secrets_get reads login fields") {
    set_fixture_env();
    hive::SecretsGetTool tool;
    StubCtx ctx;

    auto res = tool.execute({{"name", "ST61_LOGIN"}, {"field", "username"}}, ctx);
    CHECK(res.value("status", "") == "ok");
    CHECK(res.value("value", "") == "alice@example.com");

    res = tool.execute({{"name", "ST61_LOGIN"}, {"field", "password"}}, ctx);
    CHECK(res.value("value", "") == "hunter2");

    res = tool.execute({{"name", "ST61_LOGIN"}, {"field", "url"}}, ctx);
    CHECK(res.value("value", "") == "https://example.com/login");

    // Bare get on a login secret steers to the field argument.
    res = tool.execute({{"name", "ST61_LOGIN"}}, ctx);
    CHECK(res.value("status", "") == "error");
    CHECK(res.value("error", "").find("login secret") != std::string::npos);
    CHECK(res["available_fields"] ==
          json::array({"username", "password", "url"}));
    clear_fixture_env();
}

TEST_CASE("secrets_get rejects the ungranted and the malformed") {
    hive::SecretsGetTool tool;
    StubCtx ctx;

    auto res = tool.execute({{"name", "ST61_NEVER_GRANTED"}}, ctx);
    CHECK(res.value("status", "") == "error");
    CHECK(res.value("error", "").find("not granted") != std::string::npos);

    res = tool.execute(json::object(), ctx);
    CHECK(res.value("status", "") == "error");

    res = tool.execute({{"name", "bad-name!"}}, ctx);
    CHECK(res.value("status", "") == "error");

    res = tool.execute({{"name", "ST61_TOKEN"}, {"field", "bogus"}}, ctx);
    CHECK(res.value("status", "") == "error");
    CHECK(res.value("error", "").find("field must be") != std::string::npos);
}

}  // TEST_SUITE
