#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <map>

namespace mcp {

// Transport kinds we understand. Anything not listed here is treated as stdio
// (the historical default), so legacy servers.json files that omit "transport"
// keep working.
enum class TransportKind : std::uint8_t {
    Stdio,            // Spawn a subprocess; talk JSON-RPC over stdin/stdout.
    StreamableHttp,   // POST JSON-RPC to a URL; expect JSON response (MCP 2025-06-18).
    Http,             // Legacy alias for streamable-HTTP.
};

// Convert a string from servers.json (e.g. "streamable-http", "http", "stdio")
// to a TransportKind. Returns Stdio for empty/unknown values (back-compat).
inline TransportKind parse_transport_kind(const std::string& s) {
    if (s.empty()) return TransportKind::Stdio;
    if (s == "stdio") return TransportKind::Stdio;
    if (s == "streamable-http" || s == "streamable_http" ||
        s == "http" || s == "https" || s == "sse") {
        return TransportKind::StreamableHttp;
    }
    return TransportKind::Stdio;
}

// Server configuration loaded from servers.json
struct McpServerConfig {
    std::string name;                              // Server identifier (key in mcpServers)
    // --- stdio transport fields (existing) ---
    std::string command;                           // Command to launch (e.g., "uvx", "npx")
    std::vector<std::string> args;                 // Command arguments
    std::map<std::string, std::string> env;        // Environment variables
    // --- http transport fields (new) ---
    std::string transport;                         // "stdio" | "streamable-http" | "http"
    std::string url;                               // HTTP endpoint (e.g. "http://127.0.0.1:8765/mcp")
    std::string bearer_token;                      // Optional Authorization: Bearer <token>
    // --- meta ---
    std::string description;                       // Human-readable description
    bool enabled = true;                           // Whether to connect on startup

    // Transport predicates. Default = stdio (back-compat).
    [[nodiscard]] bool is_stdio() const { return parse_transport_kind(transport) == TransportKind::Stdio; }
    [[nodiscard]] bool is_http()  const { return parse_transport_kind(transport) == TransportKind::StreamableHttp; }

    // Check if this server is properly configured.
    // A server is valid if it has:
    //   - stdio: non-empty name AND non-empty command.
    //            `args` is optional — many CLI tools (e.g. /bin/true,
    //            `node`, `cat`) accept zero arguments. Requiring a
    //            non-empty args vector caused real, legitimate servers
    //            to be silently dropped on both the JSON and SQLite
    //            load paths; see test_mcp_load_from_agent.cpp (alpha
    //            server has no args) for the regression that surfaced
    //            this.
    //   - http:  non-empty name AND non-empty url.
    [[nodiscard]] bool is_valid() const {
        if (name.empty()) return false;
        if (is_http())    return !url.empty();
        return !command.empty();
    }
};

} // namespace mcp
