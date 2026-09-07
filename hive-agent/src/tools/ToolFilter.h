/**
 * @file ToolFilter.h
 * @brief Per-agent filtering of the global tool surface.
 *
 * The global ToolFactory is shared across all Agent instances, but each host
 * may want to hide different tools / MCP servers at runtime.  ToolFilter holds
 * the per-agent disabled sets and decides which tool definitions the LLM sees
 * and which tool calls are allowed through.
 */
#pragma once

#include <nlohmann/json.hpp>
#include <set>
#include <string>

class ToolFilter {
public:
    /// Names of individual tools (exact match) that are disabled.
    std::set<std::string> disabled_tools;

    /// Names of MCP servers whose proxied tools are disabled.
    std::set<std::string> disabled_mcp_servers;

    /// @brief True if `tool_name` may be exposed to the LLM / executed.
    [[nodiscard]] bool is_tool_allowed(const std::string& tool_name) const;

    /// @brief Return a copy of `all_defs` with disabled tools removed.
    ///        `all_defs` is the JSON array produced by ToolFactory.
    [[nodiscard]] nlohmann::json filter_definitions(
        const nlohmann::json& all_defs) const;

    /// @brief Return the MCP server prefix embedded in an MCP-proxied tool
    ///        name, or empty if the name is not of the form
    ///        `mcp__<server>__<tool>`.
    [[nodiscard]] static std::string extract_mcp_server(
        const std::string& tool_name);
};
