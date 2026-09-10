// ============================================================================
// tests/test_mcp_proxied_tool.cpp
// §18.9 — Tests for the per-tool MCP adapter (`McpProxiedTool`) and the
// dirty-flag/refactor lifecycle that drives it.
//
// These tests are stdlib-only — they assert *naming* and *schema* invariants
// without standing up a real MCP subprocess (the manager itself is exercised
// end-to-end in test_mcp_server_config_http.cpp).
// ============================================================================

#include "tools/McpProxiedTool.h"
#include "mcp/McpServerManager.h"
#include "mcp/McpServerConfig.h"
#include "mcp/McpClient.h"

#include <cassert>
#include <iostream>
#include <string>

namespace {

// §18.9 — Name construction. The Claude-Code-compatible namespace pattern
// MUST hold for any discovered MCP tool.
void test_name_construction() {
    mcp::ToolDefinition def;
    def.name = "kb_search";
    def.description = "Search the knowledge base";
    def.input_schema = nlohmann::json::parse(
        R"({"type":"object","properties":{"q":{"type":"string"}},"required":["q"]})");

    McpProxiedTool tool("apollocore", def, nullptr);

    assert(tool.get_name() == "mcp__apollocore__kb_search");
    assert(tool.get_description() == "Search the knowledge base");
    std::cout << "[ok] test_name_construction" << std::endl;
}

// §18.9 — Schema round-trip: get_definition().parameters must equal
// input_schema byte-for-byte so the model sees the JSON-Schema the server
// published, not a synthesised wrapper.
void test_schema_roundtrip() {
    mcp::ToolDefinition def;
    def.name = "kb_index";
    def.description = "Index a repo";
    def.input_schema = nlohmann::json::parse(
        R"({"type":"object","properties":{"path":{"type":"string"},"depth":{"type":"integer"}},"additionalProperties":false})");

    McpProxiedTool tool("apollocore", def, nullptr);
    auto def_json = tool.get_definition();
    assert(def_json.contains("type"));
    assert(def_json["type"] == "function");
    assert(def_json["function"]["name"] == "mcp__apollocore__kb_index");
    assert(def_json["function"]["parameters"] == def.input_schema);
    std::cout << "[ok] test_schema_roundtrip" << std::endl;
}

// §18.9 — Name sanitisation: spaces, slashes, etc. become underscores so
// the result is OpenAI-function-name-valid (no quoting needed).
void test_name_sanitisation() {
    mcp::ToolDefinition def;
    def.name = "weird/name with spaces!";
    def.input_schema = nlohmann::json::object();
    McpProxiedTool tool("srv", def, nullptr);
    auto got = tool.get_name();
    assert(got == "mcp__srv__weird_name_with_spaces_");
    std::cout << "[ok] test_name_sanitisation: " << got << std::endl;
}

// §18.9 — Two servers exposing the same tool name MUST produce distinct
// tool names — no global collision.
void test_namespace_disambiguation() {
    mcp::ToolDefinition def;
    def.name = "search";
    def.input_schema = nlohmann::json::object();

    McpProxiedTool a("a", def, nullptr);
    McpProxiedTool b("b", def, nullptr);
    assert(a.get_name() == "mcp__a__search");
    assert(b.get_name() == "mcp__b__search");
    assert(a.get_name() != b.get_name());
    std::cout << "[ok] test_namespace_disambiguation" << std::endl;
}

// §18.9 — Dirty flag plumbing on the manager. Toggling it must round-trip
// through the public API and reset cleanly.
void test_dirty_flag_plumbing() {
    auto& mgr = mcp::McpServerManager::instance();
    // Manager's initial state has dirty=true (so first refresh picks up
    // any pre-existing connection state).
    bool initial = mgr.mcp_tools_dirty();
    mgr.mark_mcp_tools_dirty();
    assert(mgr.mcp_tools_dirty() == true);
    mgr.clear_mcp_tools_dirty();
    assert(mgr.mcp_tools_dirty() == false);
    // Restore initial value so subsequent tests don't observe surprise.
    if (initial) mgr.mark_mcp_tools_dirty();
    std::cout << "[ok] test_dirty_flag_plumbing" << std::endl;
}

// §18.9 — Token-count snapshot. Even with no servers configured the call
// must return 0 (no crash), and we expect the schema to be JSON-empty.
void test_estimated_token_count_empty() {
    auto& mgr = mcp::McpServerManager::instance();
    // Token estimate is 0 if nothing has ever been refreshed.
    size_t n = mgr.estimated_token_count();
    // We don't assert a specific number — it depends on whether prior
    // tests have populated the schema — but it must NOT be huge.
    assert(n < 1000000);
    std::cout << "[ok] test_estimated_token_count_empty (n=" << n << ")" << std::endl;
}

// §18.9 — Public API surface is what callers depend on. If any of these
// disappear, build breaks here before it breaks in production.
void test_public_api_surface() {
    auto& mgr = mcp::McpServerManager::instance();
    // Compile-time assertions: if any of these symbols goes away, this
    // test will fail to build, which is the desired failure mode.
    auto v = mgr.list_configured_servers();
    (void)v;
    (void)mgr.is_healthy("nonexistent");      // returns false
    (void)mgr.resolve("nonexistent", "x");    // returns nullptr
    (void)mgr.estimated_token_count();        // returns 0
    (void)mgr.mcp_tools_dirty();
    (void)mgr.is_config_loaded();
    (void)mgr.get_tools("nonexistent");       // returns empty vec
    std::cout << "[ok] test_public_api_surface" << std::endl;
}

}  // namespace

int main() {
    test_name_construction();
    test_schema_roundtrip();
    test_name_sanitisation();
    test_namespace_disambiguation();
    test_dirty_flag_plumbing();
    test_estimated_token_count_empty();
    test_public_api_surface();
    std::cout << "test_mcp_proxied_tool: 7 tests passed" << std::endl;
    return 0;
}