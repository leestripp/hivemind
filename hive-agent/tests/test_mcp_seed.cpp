// tests/test_mcp_seed.cpp
//
// Phase 71: hub-roster MCP seed merge semantics (core/McpSeed).
//   SEED1. Hub servers are inserted with origin='hub'.
//   SEED2. Re-seed drops undeclared hub rows, upserts declared ones.
//   SEED3. origin='local' rows (agent self-service) survive re-seeds.
//   SEED4. Empty seed still drops stale hub rows (cleared roster).
//   SEED5. Pre-v6 DB (no origin column): rows marked 'hub' on upgrade.
//   SEED6. http servers (url, no command) seed with streamable-http.

#include <chrono>
#include <filesystem>
#include <string>
#include <unistd.h>

#include <nlohmann/json.hpp>

#include "core/McpSeed.h"
#include "core/settings.h"
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest/doctest.h"

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {

struct TmpHome {
    fs::path path;
    TmpHome() {
        path = fs::temp_directory_path() /
               ("hive-seed-" + std::to_string(std::chrono::system_clock::now()
                                .time_since_epoch().count()) + "-" +
                std::to_string(::getpid()));
        fs::remove_all(path);
        fs::create_directories(path);
    }
    ~TmpHome() { std::error_code ec; fs::remove_all(path, ec); }
};

void quiet_log(const std::string&) {}

void seed(const fs::path& home, const json& servers) {
    hive::mcp_seed::seed_hub_mcp_servers(home, servers, quiet_log);
}

// Open the seeded db through the canonical store API (auto-migrate so
// the v6 origin column is guaranteed present even on pre-v6 fixtures).
std::unique_ptr<hive::SettingsStore> open_store(const fs::path& home) {
    return std::make_unique<hive::SettingsStore>(
        hive::SettingsStore::for_home(home));
}

json hub(std::initializer_list<json> servers) {
    json arr = json::array();
    for (const auto& s : servers) arr.push_back(s);
    return arr;
}

}  // namespace

TEST_CASE("SEED1: hub servers inserted with origin=hub") {
    TmpHome home;
    seed(home.path, hub({
        {{"name", "blender"}, {"command", "blender-mcp"}},
    }));
    auto store = open_store(home.path);
    auto row = store->get_mcp_server("blender");
    REQUIRE(row.has_value());
    CHECK(row->origin == "hub");
    CHECK(row->command == "blender-mcp");
    CHECK(row->transport == "stdio");
    CHECK(row->enabled);
}

TEST_CASE("SEED2/3: re-seed drops undeclared hub rows, preserves local") {
    TmpHome home;
    seed(home.path, hub({
        {{"name", "alpha"}, {"command", "alpha-mcp"}},
        {{"name", "beta"},  {"command", "beta-mcp"}},
    }));
    // Agent self-service row (what mcp_add writes).
    {
        auto store = open_store(home.path);
        hive::SettingsStore::McpServer local;
        local.name = "comfy";
        local.command = "comfy-mcp";
        local.origin = "local";
        store->upsert_mcp_server(local);
    }
    // Re-seed: alpha stays, beta is gone from the roster, gamma is new.
    seed(home.path, hub({
        {{"name", "alpha"}, {"command", "alpha-mcp-v2"}},
        {{"name", "gamma"}, {"command", "gamma-mcp"}},
    }));
    auto store = open_store(home.path);
    auto alpha = store->get_mcp_server("alpha");
    REQUIRE(alpha.has_value());
    CHECK(alpha->command == "alpha-mcp-v2");  // upserted
    CHECK(alpha->origin == "hub");
    CHECK(!store->get_mcp_server("beta").has_value());  // dropped
    auto gamma = store->get_mcp_server("gamma");
    REQUIRE(gamma.has_value());
    CHECK(gamma->origin == "hub");
    auto comfy = store->get_mcp_server("comfy");        // PRESERVED
    REQUIRE(comfy.has_value());
    CHECK(comfy->origin == "local");
    CHECK(comfy->command == "comfy-mcp");
}

TEST_CASE("SEED4: empty seed drops stale hub rows, keeps local") {
    TmpHome home;
    seed(home.path, hub({
        {{"name", "blender"}, {"command", "blender-mcp"}},
    }));
    {
        auto store = open_store(home.path);
        hive::SettingsStore::McpServer local;
        local.name = "comfy";
        local.command = "comfy-mcp";
        local.origin = "local";
        store->upsert_mcp_server(local);
    }
    seed(home.path, json::array());  // roster cleared
    auto store = open_store(home.path);
    CHECK(!store->get_mcp_server("blender").has_value());
    REQUIRE(store->get_mcp_server("comfy").has_value());
}

TEST_CASE("SEED5: pre-v6 DB rows become origin=hub on upgrade") {
    TmpHome home;
    // Build a pre-v6 db by hand: 9-column schema, no origin.
    {
        OmniNet::DB::Database db;
        db.open((home.path / "hive-agent.db").string());
        db.execute(
            "CREATE TABLE mcp_servers ("
            " name TEXT PRIMARY KEY, transport TEXT, command TEXT,"
            " args_json TEXT, env_json TEXT, url TEXT, bearer_token TEXT,"
            " description TEXT, enabled INTEGER DEFAULT 1)");
        auto st = db.prepare(
            "INSERT INTO mcp_servers (name, transport, command, enabled)"
            " VALUES ('oldhub', 'stdio', 'old-mcp', 1)");
        st.step();
    }
    // Seed with a DIFFERENT roster: oldhub must be dropped (it was a
    // hub row), newhub inserted.
    seed(home.path, hub({
        {{"name", "newhub"}, {"command", "new-mcp"}},
    }));
    auto store = open_store(home.path);
    CHECK(!store->get_mcp_server("oldhub").has_value());
    auto nh = store->get_mcp_server("newhub");
    REQUIRE(nh.has_value());
    CHECK(nh->origin == "hub");
}

TEST_CASE("SEED6: url-only server seeds as streamable-http") {
    TmpHome home;
    seed(home.path, hub({
        {{"name", "remote"}, {"url", "http://127.0.0.1:8765/mcp"}},
    }));
    auto store = open_store(home.path);
    auto row = store->get_mcp_server("remote");
    REQUIRE(row.has_value());
    CHECK(row->transport == "streamable-http");
    CHECK(row->url == "http://127.0.0.1:8765/mcp");
    CHECK(row->origin == "hub");
}
