/**
 * @file McpSeed.h
 * @brief Phase 71: hub-roster MCP seed with origin ownership.
 *
 * Extracted from main.cpp so the merge semantics are unit-testable.
 * The seed manages ONLY origin='hub' rows: declared hub servers are
 * upserted, undeclared hub rows are dropped, and origin='local' rows
 * (agent self-service via the mcp_add tool) are PRESERVED across
 * respawns.
 */
#pragma once

#include <filesystem>
#include <functional>
#include <string>

#include <nlohmann/json.hpp>

namespace hive::mcp_seed {

/// Seed hub-declared MCP servers into <home>/hive-agent.db.
/// `servers` is the parsed HIVEMIND_MCP_SERVERS JSON array (may be
/// empty — hub rows are still dropped so a cleared roster takes
/// effect). `log` receives human-readable diagnostics (may be a
/// no-op). Never throws: all failures are logged and swallowed — a
/// broken seed must not prevent the brain from booting.
void seed_hub_mcp_servers(
    const std::filesystem::path& home,
    const nlohmann::json& servers,
    const std::function<void(const std::string&)>& log);

}  // namespace hive::mcp_seed
