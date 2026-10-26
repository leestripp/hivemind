/**
 * @file McpManageTools.h
 * @brief Phase 71: agent self-service MCP server management.
 *
 * Five builtin tools exposing McpServerManager + SettingsStore to the
 * LLM so an agent can install, wire, enable, disable and remove MCP
 * servers inside its own container without hub involvement:
 *
 *   mcp_list     — rows + live connection state (env values redacted)
 *   mcp_add      — persist (origin='local') + connect live
 *   mcp_enable   — flip enabled + connect (allowed on hub rows too:
 *                  reconnect after a transient failure)
 *   mcp_disable  — disconnect + persist enabled=0 (local rows only)
 *   mcp_remove   — disconnect + delete row (local rows only)
 *
 * Hub-roster rows (origin='hub') are hub-authoritative: disable/remove
 * are refused with a clear message. mcp_add/mcp_enable spawn a child
 * process (stdio) or open a connection (http) — same risk class as
 */
#pragma once

#include "tools/BaseTool.h"

namespace mcp { class McpServerManager; }
namespace hive { class SettingsStore; }

/// Shared service handles for the MCP management tools. Both pointers
/// are borrowed from AgentImpl's Body and outlive the tools (the
/// factory holds tools for the Agent's lifetime).
struct McpToolServices {
    mcp::McpServerManager* mgr   = nullptr;   // never null when registered
    hive::SettingsStore*   store = nullptr;   // never null when registered
};

class McpListTool : public BaseTool {
public:
    explicit McpListTool(McpToolServices svc) : svc_(svc) {}
    [[nodiscard]] std::string get_name() const override { return "mcp_list"; }
    [[nodiscard]] std::string get_description() const override;
    [[nodiscard]] json get_definition() const override;
    json execute(const json& args, FileIO& file_io) override;
private:
    McpToolServices svc_;
};

class McpAddTool : public BaseTool {
public:
    explicit McpAddTool(McpToolServices svc) : svc_(svc) {}
    [[nodiscard]] std::string get_name() const override { return "mcp_add"; }
    [[nodiscard]] std::string get_description() const override;
    [[nodiscard]] json get_definition() const override;
    json execute(const json& args, FileIO& file_io) override;
private:
    McpToolServices svc_;
};

class McpEnableTool : public BaseTool {
public:
    explicit McpEnableTool(McpToolServices svc) : svc_(svc) {}
    [[nodiscard]] std::string get_name() const override { return "mcp_enable"; }
    [[nodiscard]] std::string get_description() const override;
    [[nodiscard]] json get_definition() const override;
    json execute(const json& args, FileIO& file_io) override;
private:
    McpToolServices svc_;
};

class McpDisableTool : public BaseTool {
public:
    explicit McpDisableTool(McpToolServices svc) : svc_(svc) {}
    [[nodiscard]] std::string get_name() const override { return "mcp_disable"; }
    [[nodiscard]] std::string get_description() const override;
    [[nodiscard]] json get_definition() const override;
    json execute(const json& args, FileIO& file_io) override;
private:
    McpToolServices svc_;
};

class McpRemoveTool : public BaseTool {
public:
    explicit McpRemoveTool(McpToolServices svc) : svc_(svc) {}
    [[nodiscard]] std::string get_name() const override { return "mcp_remove"; }
    [[nodiscard]] std::string get_description() const override;
    [[nodiscard]] json get_definition() const override;
    json execute(const json& args, FileIO& file_io) override;
private:
    McpToolServices svc_;
};
