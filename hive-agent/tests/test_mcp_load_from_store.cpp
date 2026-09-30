// SPDX-License-Identifier: BSL-1.0
// tests/test_mcp_load_from_store.cpp
//
// Verifies McpServerManager::load_from_store() correctly ingests
// SettingsStore::McpServer rows into the same configs_ map that
// load_config() populates from servers.json.
//
//   ML1. Round-trip: write rows via SettingsStore.upsert_mcp_server,
//       call McpServerManager::load_from_store(), then verify
//       list_configured_servers() returns the same names.
//   ML2. HTTP transport fields survive the round-trip.
//   ML3. Bearer token survives the round-trip.
//   ML4. Args/env survive the round-trip.
//   ML5. Disabled-server gate (Phase 15) is honoured.

#include <cassert>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <string>
#include <unistd.h>

#include "core/settings.h"
#include "mcp/McpServerManager.h"
#include "mcp/McpServerConfig.h"

namespace fs = std::filesystem;

int main() {
    int failures = 0;
#define EXPECT(cond) do {                                              \
    if (!(cond)) {                                                      \
        std::cerr << "FAIL @ " << __LINE__ << ": " #cond << "\n";       \
        ++failures;                                                     \
    }                                                                   \
} while (0)

    // Per-test isolated DB.
    auto path = fs::temp_directory_path() /
                ("daedalus-ml-" +
                 std::to_string(std::chrono::system_clock::now()
                                  .time_since_epoch().count()) + "-" +
                 std::to_string(::getpid()));
    fs::remove_all(path);
    fs::create_directories(path);
    {
        std::error_code ec;
        auto store = hive::SettingsStore::for_home(path);

        // ML1: stdio server
        hive::SettingsStore::McpServer s1;
        s1.name      = "test-stdio";
        s1.transport = "stdio";
        s1.command   = "/bin/echo";
        s1.args      = {"hello", "world"};
        s1.description = "test";
        s1.enabled   = true;
        store.upsert_mcp_server(s1);

        // ML2: http server
        hive::SettingsStore::McpServer s2;
        s2.name         = "test-http";
        s2.transport    = "streamable-http";
        s2.url          = "http://localhost:9000/mcp";
        s2.bearer_token = "Bearer secret-xyz";
        s2.description  = "test http";
        s2.enabled      = true;
        store.upsert_mcp_server(s2);

        // Now load them via McpServerManager
        mcp::McpServerManager mgr;
        EXPECT(mgr.load_from_store(store));

        auto names = mgr.list_configured_servers();
        EXPECT(names.size() == 2);
        bool found_stdio = false, found_http = false;
        for (const auto& n : names) {
            if (n == "test-stdio") found_stdio = true;
            if (n == "test-http")  found_http = true;
        }
        EXPECT(found_stdio);
        EXPECT(found_http);

        // ML2: HTTP transport fields
        const auto* http_cfg = mgr.get_config("test-http");
        EXPECT(http_cfg != nullptr);
        if (http_cfg) {
            EXPECT(http_cfg->transport == "streamable-http");
            EXPECT(http_cfg->url == "http://localhost:9000/mcp");
        }

        // ML3: bearer_token
        if (http_cfg) {
            EXPECT(http_cfg->bearer_token == "Bearer secret-xyz");
        }

        // ML4: args/env
        const auto* stdio_cfg = mgr.get_config("test-stdio");
        EXPECT(stdio_cfg != nullptr);
        if (stdio_cfg) {
            EXPECT(stdio_cfg->args.size() == 2);
            if (stdio_cfg->args.size() == 2) {
                EXPECT(stdio_cfg->args[0] == "hello");
                EXPECT(stdio_cfg->args[1] == "world");
            }
            EXPECT(stdio_cfg->command == "/bin/echo");
        }

        // ML5: disabled gate
        mgr.set_disabled_servers({"test-http"});
        EXPECT(mgr.load_from_store(store));
        names = mgr.list_configured_servers();
        EXPECT(names.size() == 1);
        EXPECT(names[0] == "test-stdio");

        // ML6 (2026-09-30): the db row's OWN enabled flag must gate too.
        // Pre-fix only the config disabled-list was honored — an
        // mcp_disable'd server (enabled=0 in the store) reconnected on
        // every brain restart, resurrecting whatever made it need
        // disabling (Bob's schema-poisoned comfy server re-muted him).
        mgr.set_disabled_servers({});
        hive::SettingsStore::McpServer s3;
        s3.name      = "test-store-disabled";
        s3.transport = "stdio";
        s3.command   = "/bin/echo";
        s3.args      = {"nope"};
        s3.enabled   = false;
        store.upsert_mcp_server(s3);
        EXPECT(mgr.load_from_store(store));
        names = mgr.list_configured_servers();
        EXPECT(names.size() == 2);
        for (const auto& n : names)
            EXPECT(n != "test-store-disabled");
    }
    fs::remove_all(path);

    if (failures) {
        std::cerr << "\ntest_mcp_load_from_store: " << failures
                  << " FAILED\n";
        return 1;
    }
    std::cout << "\ntest_mcp_load_from_store: all tests passed\n";
    return 0;
}