// SPDX-License-Identifier: BSL-1.0
// tests/test_mcp_load_from_agent.cpp
//
// §FIX-MCP-STORE-LOAD (plan R3-P1, 2026-07-17) end-to-end regression.
//
// Two related bugs:
//   (a) McpServerManager::load_from_store() was dead code — added in
//       v0.5.0 to read from daedalus.db's mcp_servers table but never
//       called by AgentImpl, which only invoked load_config() (the
//       legacy JSON path).
//   (b) The MCP setup was inside `if (has_explicit_creds)` — meaning
//       if the store had MCP servers but no default model, MCP setup
//       was silently skipped entirely. MCP is independent of the LLM;
//       it should be gated on `cfg.mcp_enabled` only.
//
// This test seeds a per-test SettingsStore via
// SettingsStore::upsert_mcp_server (the documented v0.5.0 API),
// constructs an Agent pointing at that store, and asserts the
// server names are visible via Agent::mcp_configured_servers().
//
// No network connections are attempted — the stdio command is
// /bin/true which exits immediately. The test only verifies that the
// SERVERS got LOADED into the McpServerManager, not that they
// successfully connect.

#include <cassert>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <string>
#include <unistd.h>

#include "library/Agent.h"
#include "core/settings.h"

namespace fs = std::filesystem;

namespace {

fs::path fresh_home_dir() {
    return fs::temp_directory_path() /
           ("daedalus-mcp-agent-e2e-" +
            std::to_string(std::chrono::system_clock::now()
                                .time_since_epoch()
                                .count()) +
            "-" + std::to_string(::getpid()));
}

}  // namespace

int main() {
    int failures = 0;
#define EXPECT(cond) do {                                              \
    if (!(cond)) {                                                      \
        std::cerr << "FAIL @ " << __LINE__ << ": " #cond << "\n";       \
        ++failures;                                                     \
    }                                                                   \
} while (0)

    auto home = fresh_home_dir();
    fs::remove_all(home);
    fs::create_directories(home);

    // Phase 1: seed the store with two MCP servers BEFORE constructing
    // Agent. This mirrors the user's report: "added MCP server via the
    // documented v0.5.0 API and it doesn't work".
    {
        auto store = hive::SettingsStore::for_home(home);

        hive::SettingsStore::McpServer s1;
        s1.name        = "alpha-server";
        s1.transport   = "stdio";
        s1.command     = "/bin/true";
        s1.description = "e2e test: alpha";
        s1.enabled     = true;
        store.upsert_mcp_server(s1);

        hive::SettingsStore::McpServer s2;
        s2.name        = "beta-server";
        s2.transport   = "stdio";
        s2.command     = "/bin/true";
        s2.args        = {"--version"};
        s2.description = "e2e test: beta";
        s2.enabled     = true;
        store.upsert_mcp_server(s2);
    }

    // Phase 2: construct the Agent. The bug used to live here — the
    // MCP setup was inside has_explicit_creds, so when no model was
    // configured, MCP servers were silently dropped. Now MCP setup is
    // gated only on cfg.mcp_enabled and calls load_from_store().
    hive::AgentConfig cfg;
    cfg.agent_home = home;
    cfg.mcp_enabled   = true;
    // Deliberately do NOT set cfg.model_name or cfg.api_key — the test
    // covers "store has MCP servers but no LLM configured".
    hive::Agent agent(cfg, /*sink=*/nullptr);
    // MCP setup happens in AgentImpl::initialize(), NOT in the ctor
    // (matches the documented Agent lifecycle: ctor = cheap, init = heavy).
    agent.initialize();

    // Phase 3: verify the public Agent surface reports both servers.
    auto configured = agent.mcp_configured_servers();
    bool saw_alpha = false;
    bool saw_beta  = false;
    for (const auto& name : configured) {
        if (name == "alpha-server") saw_alpha = true;
        if (name == "beta-server")  saw_beta  = true;
    }
    EXPECT(saw_alpha);
    EXPECT(saw_beta);
    EXPECT(configured.size() == 2);

    std::error_code ec;
    fs::remove_all(home, ec);

    if (failures == 0) {
        std::cout << "[ok] MCP servers loaded from SettingsStore by Agent::Agent()\n";
        return 0;
    }
    std::cerr << failures << " test failure(s)\n";
    return 1;
}
