// SPDX-License-Identifier: BSL-1.0
// tests/test_mcp_servers_store.cpp
//
// Verifies the v0.5.0 MCP server registry in SettingsStore:
//   MS1. Upsert + get round-trip on a stdio server.
//   MS2. Upsert + get round-trip on an HTTP server with bearer_token.
//   MS3. Args + env serialize/deserialize correctly.
//   MS4. list_mcp_servers() returns all rows.
//   MS5. delete_mcp_server removes the row.
//   MS6. has_mcp_servers() reflects the table state.
//   MS7. Upsert of HTTP server with empty args (zero-arg stdio regression).

#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <unistd.h>

#include <nlohmann/json.hpp>

#include "core/settings.h"

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {

struct TmpHome {
    fs::path path;
    TmpHome(const std::string& tag) {
        path = fs::temp_directory_path() /
               ("daedalus-ms-" + tag + "-" +
                std::to_string(std::chrono::system_clock::now()
                                 .time_since_epoch().count()) + "-" +
                std::to_string(::getpid()));
        fs::remove_all(path);
        fs::create_directories(path / "config_root");
        fs::create_directories(path / "config_root" / "mcp");
        fs::create_directories(path / "data");
    }
    ~TmpHome() { std::error_code ec; fs::remove_all(path, ec); }

    fs::path config_root() const { return path / "config_root"; }
    fs::path data_root()    const { return path / "data"; }
    fs::path config_json() const { return config_root() / "config.json"; }
    fs::path mcp_json()    const { return config_root() / "mcp" / "servers.json"; }
};

int failures = 0;
#define EXPECT(cond) do {                                              \
    if (!(cond)) {                                                      \
        std::cerr << "FAIL @ " << __LINE__ << ": " #cond << "\n";       \
        ++failures;                                                     \
    }                                                                   \
} while (0)

// MS1: stdio server round-trip
void test_ms1_stdio_roundtrip() {
    TmpHome h("ms1");
    auto store = hive::SettingsStore::for_home(h.data_root());

    hive::SettingsStore::McpServer s;
    s.name        = "fs-server";
    s.transport   = "stdio";
    s.command     = "uvx";
    s.args        = {"mcp-server", "--stdio"};
    s.env["API_KEY"] = "secret-123";
    s.description = "Filesystem MCP server";
    s.enabled     = true;
    store.upsert_mcp_server(s);

    auto loaded = store.get_mcp_server("fs-server");
    EXPECT(loaded.has_value());
    if (loaded) {
        EXPECT(loaded->name == "fs-server");
        EXPECT(loaded->transport == "stdio");
        EXPECT(loaded->command == "uvx");
        EXPECT(loaded->args.size() == 2);
        if (loaded->args.size() == 2) {
            EXPECT(loaded->args[0] == "mcp-server");
            EXPECT(loaded->args[1] == "--stdio");
        }
        EXPECT(loaded->env.count("API_KEY") == 1);
        if (loaded->env.count("API_KEY")) {
            EXPECT(loaded->env.at("API_KEY") == "secret-123");
        }
        EXPECT(loaded->description == "Filesystem MCP server");
        EXPECT(loaded->enabled == true);
    }
    std::cout << "[ok] MS1: stdio server round-trip\n";
}

// MS2: HTTP server with bearer_token
void test_ms2_http_roundtrip() {
    TmpHome h("ms2");
    auto store = hive::SettingsStore::for_home(h.data_root());

    hive::SettingsStore::McpServer s;
    s.name         = "remote-mcp";
    s.transport    = "streamable-http";
    s.url          = "https://mcp.example.com/mcp";
    s.bearer_token = "Bearer xyz";
    s.description  = "Remote MCP";
    s.enabled      = true;
    store.upsert_mcp_server(s);

    auto loaded = store.get_mcp_server("remote-mcp");
    EXPECT(loaded.has_value());
    if (loaded) {
        EXPECT(loaded->transport == "streamable-http");
        EXPECT(loaded->url == "https://mcp.example.com/mcp");
        EXPECT(loaded->bearer_token == "Bearer xyz");
    }
    std::cout << "[ok] MS2: HTTP server + bearer_token round-trip\n";
}

// MS3: empty args/env
void test_ms3_empty_args_env() {
    TmpHome h("ms3");
    auto store = hive::SettingsStore::for_home(h.data_root());

    hive::SettingsStore::McpServer s;
    s.name      = "min";
    s.transport = "stdio";
    s.command   = "/bin/echo";
    s.args      = {};
    s.env       = {};
    store.upsert_mcp_server(s);

    auto loaded = store.get_mcp_server("min");
    EXPECT(loaded.has_value());
    if (loaded) {
        EXPECT(loaded->args.empty());
        EXPECT(loaded->env.empty());
    }
    std::cout << "[ok] MS3: empty args/env round-trip\n";
}

// MS4: list_mcp_servers returns all
void test_ms4_list_all() {
    TmpHome h("ms4");
    auto store = hive::SettingsStore::for_home(h.data_root());

    EXPECT(store.list_mcp_servers().empty());
    EXPECT(!store.has_mcp_servers());

    for (int i = 0; i < 3; ++i) {
        hive::SettingsStore::McpServer s;
        s.name      = "srv-" + std::to_string(i);
        s.transport = "stdio";
        s.command   = "/bin/echo";
        store.upsert_mcp_server(s);
    }

    auto all = store.list_mcp_servers();
    EXPECT(all.size() == 3);
    EXPECT(store.has_mcp_servers());
    std::cout << "[ok] MS4: list_mcp_servers returns all, has_mcp_servers reflects state\n";
}

// MS5: delete removes
void test_ms5_delete() {
    TmpHome h("ms5");
    auto store = hive::SettingsStore::for_home(h.data_root());

    hive::SettingsStore::McpServer s;
    s.name      = "to-delete";
    s.transport = "stdio";
    s.command   = "/bin/echo";
    store.upsert_mcp_server(s);
    EXPECT(store.get_mcp_server("to-delete").has_value());

    store.delete_mcp_server("to-delete");
    EXPECT(!store.get_mcp_server("to-delete").has_value());
    EXPECT(!store.has_mcp_servers());
    std::cout << "[ok] MS5: delete_mcp_server removes the row\n";
}

// MS6: upsert replaces
void test_ms6_upsert_replaces() {
    TmpHome h("ms6");
    auto store = hive::SettingsStore::for_home(h.data_root());

    hive::SettingsStore::McpServer s1;
    s1.name = "dup";
    s1.command = "/old";
    s1.transport = "stdio";
    store.upsert_mcp_server(s1);

    hive::SettingsStore::McpServer s2 = s1;
    s2.command = "/new";
    store.upsert_mcp_server(s2);

    auto loaded = store.get_mcp_server("dup");
    EXPECT(loaded.has_value());
    if (loaded) EXPECT(loaded->command == "/new");
    EXPECT(store.list_mcp_servers().size() == 1);
    std::cout << "[ok] MS6: upsert replaces existing row\n";
}

// MS7: HTTP server with bearer_token serialises correctly (HTTP store path)
// MS7: HTTP server with bearer_token + empty args (stdio zero-arg regression)
void test_ms7_http_empty_args() {
    TmpHome h("ms7");
    auto store = hive::SettingsStore::for_home(h.data_root());

    // HTTP server — url and bearer_token are required, args are irrelevant.
    hive::SettingsStore::McpServer s;
    s.name         = "http-zero-args";
    s.transport    = "streamable-http";
    s.url          = "https://mcp.example.com/mcp";
    s.bearer_token = "Bearer tok123";
    s.args         = {};  // HTTP servers don't use args — must be accepted
    s.description  = "HTTP with zero args";
    store.upsert_mcp_server(s);

    auto loaded = store.get_mcp_server("http-zero-args");
    EXPECT(loaded.has_value());
    if (loaded) {
        EXPECT(loaded->transport == "streamable-http");
        EXPECT(loaded->url == "https://mcp.example.com/mcp");
        EXPECT(loaded->bearer_token == "Bearer tok123");
        EXPECT(loaded->args.empty());  // zero args is valid for HTTP
    }
    std::cout << "[ok] MS7: HTTP server with zero args (store path)\n";
}

}  // namespace

int main() {
    test_ms1_stdio_roundtrip();
    test_ms2_http_roundtrip();
    test_ms3_empty_args_env();
    test_ms4_list_all();
    test_ms5_delete();
    test_ms6_upsert_replaces();
    test_ms7_http_empty_args();

    if (failures) {
        std::cerr << "\ntest_mcp_servers_store: " << failures
                  << " FAILED (of 7 tests)\n";
        return 1;
    }
    std::cout << "\ntest_mcp_servers_store: 7 tests passed\n";
    return 0;
}