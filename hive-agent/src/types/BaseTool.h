/**
 * @file hive-brain/types/BaseTool.h
 * @brief Base class for tools registered by the host application.
 *
 * Hosts can extend the agent's toolbox without modifying the hive-agent engine by
 * subclassing `hive::BaseTool` and registering the tool with
 * `Agent::register_tool()`. The library wraps the custom tool in an
 * internal adapter so it flows through the same dispatch path as the
 * built-in tools.
 *
 * @note This class replaces the older `hive::HostTool` name.
 *       `HostTool` remains as a deprecated alias for backward
 *       compatibility and will be removed in a future release.
 */
#pragma once

#include <nlohmann/json.hpp>
#include <string>

#include "types/HostToolContext.h"

namespace hive {

/**
 * @brief Abstract base class for host-registered agent tools.
 *
 * Override the four pure virtual functions to define a new tool:
 *   - get_name()        — the tool's identifier in JSON tool calls
 *   - get_description() — short human/LLM-readable description
 *   - get_definition()  — JSON Schema / OpenAI-style function definition
 *   - execute()         — run the tool and return a JSON result
 */
class BaseTool {
public:
    virtual ~BaseTool() = default;

    /// @brief Tool identifier. Must be unique within the agent's tool set.
    [[nodiscard]] virtual std::string get_name() const = 0;

    /// @brief Short description surfaced to the LLM in the system prompt.
    [[nodiscard]] virtual std::string get_description() const = 0;

    /// @brief OpenAI-compatible function definition (JSON object).
    [[nodiscard]] virtual nlohmann::json get_definition() const = 0;

    /// @brief Execute the tool.
    /// @param args  Arguments parsed from the LLM's tool_call.
    /// @param ctx   Project-scoped I/O context (read/write files, list dirs).
    /// @return      JSON result object; should contain a `"status"` field.
    virtual nlohmann::json execute(const nlohmann::json& args,
                                   HostToolContext& ctx) = 0;
};

}  // namespace hive
