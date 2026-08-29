#pragma once

// =============================================================================
// mcp_bridge — Phase 33 (worker side): MCP server processes running INSIDE
// agent containers, bridged to the hive-server over the worker channel.
//
// One process per (agent, server): spawned lazily on first rpc as
//   podman exec -i [-e K=V ...] hive-<agent> <command> <args...>
// MCP stdio framing is newline-delimited JSON-RPC: each request payload is
// written as one line; we read lines until the response carrying the
// request's "id" arrives (notifications carry no id → write only).
// rpcs per (agent, server) are serialized on the process mutex — MCP stdio
// is strictly sequential. Idle processes are reaped after 10 minutes.
// =============================================================================

#include <string>
#include <vector>
#include <nlohmann/json.hpp>

namespace hwbridge {

/// Synchronous JSON-RPC round trip. Returns the response line on success.
/// On failure returns "" and sets err.
std::string rpc(const std::string& agent, const std::string& server,
                const std::string& command,
                const std::vector<std::string>& args,
                const nlohmann::json& env,
                const std::string& payload,
                int timeout_s, std::string& err);

/// Kill processes idle longer than 10 min. Called from the heartbeat loop.
void reap_idle();

/// Kill all MCP processes (worker shutdown).
void shutdown_all();

} // namespace hwbridge
