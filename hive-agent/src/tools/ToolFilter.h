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

/**
 * @brief Interface for tools that need the calling agent's ToolFilter at
 *        execution time (§LIST-TOOLS 2026-09-20).
 *
 * ToolFactory's filter-aware execute_tool() overload dynamic_casts the
 * resolved tool to this interface and hands it the filter for the duration
 * of the call. This avoids process-global filter state — the global
 * ToolFactory is shared across Agent instances, so a static "current
 * filter" would leak one agent's policy into another agent's answers.
 */
class FilterAwareTool {
public:
    virtual ~FilterAwareTool() = default;

    /// Called immediately before execute(); pointer is valid only for the
    /// duration of that call. Never invoked on the unfiltered overloads.
    virtual void set_active_filter(const ToolFilter* filter) = 0;
};
