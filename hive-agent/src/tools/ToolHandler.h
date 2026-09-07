#pragma once

#include <string>
#include <vector>
#include <nlohmann/json.hpp>
#include <filesystem>
#include <memory>

#include "BaseTool.h"  // for ToolProgressFn
#include "library/AgentSink.h"
#include "ToolFilter.h"

class FileIO;  // Forward declaration
class ToolFactory;
class SessionMemory;

using json = nlohmann::json;

namespace ToolHandler {
    // Initialize the tool factory
    void init_tool_factory();

    // Initialize memory tools with SessionMemory pointer
    void init_memory_tools(SessionMemory* session_memory);

    // Set auto mode (automatically approve tool executions)
    void set_auto_mode(bool enabled);
    bool is_auto_mode_enabled();

    // §BUG-008 — opt out of auto-recording approved commands into the
    // session-approved cache. Default true (legacy behaviour: every
    // host-approved command is remembered for the rest of the process).
    void set_remember_approvals(bool enabled);
    bool remember_approvals_enabled();

    // §Phase 3 — set the AgentSink that tools (e.g. execute_bash) use
    // for blocking I/O like command approval. Pass nullptr to clear.
    // ToolHandler keeps a weak_ptr so the sink lifetime is owned by the host.
    void set_sink(std::shared_ptr<hive::AgentSink> sink);

    // §Phase 3 — request approval from the host's sink. Returns true if
    // approved, false otherwise. If no sink is attached, returns the value
    // of is_auto_mode_enabled() (preserving the legacy behaviour).
    bool request_command_approval(const std::string& command);

    // Execute a tool call - delegates to ToolFactory
    json execute_tool_call(const json& tool_call, FileIO& file_io);

    // Execute a tool call with an optional progress callback. See
    // ToolFactory::execute_tool() and BaseTool::execute(args, file_io, progress).
    json execute_tool_call(const json& tool_call,
                           FileIO& file_io,
                           ToolProgressFn progress);

    // Execute a tool call with optional progress callback and per-agent filter.
    json execute_tool_call(const json& tool_call,
                           FileIO& file_io,
                           ToolProgressFn progress,
                           const ToolFilter& filter);
    
    // Get tool definitions as JSON string
    std::string get_tool_definitions();

    // Get tool definitions filtered by a per-agent ToolFilter
    std::string get_tool_definitions_filtered(const ToolFilter& filter);

    // Get tool definitions excluding specified tools (for sub-agent mode)
    std::string get_tool_definitions_filtered(const std::vector<std::string>& exclude_tools);
    
    // Approval management
    void clear_session_approved_commands();
    void load_persisted_approvals(const std::filesystem::path& project_path,
                                  std::string_view agent_name = {});
    void clear_persisted_approvals(const std::filesystem::path& project_path,
                                   std::string_view agent_name = {});
    void add_persisted_approval(const std::filesystem::path& project_path,
                                std::string_view agent_name,
                                const std::string& command);
    bool matches_pattern(const std::string& command, const std::string& pattern);
    
    // Current working directory management
    void init_current_working_directory(const std::filesystem::path& project_root);
    std::filesystem::path get_current_working_directory();
    bool is_cwd_initialized();
    
    // Check if command is session approved (for execute_bash)
    bool is_command_session_approved(const std::string& command);
    
    // Internal: Add command to session approved (used by BashTool for chained commands)
    void add_session_approved_command_internal(const std::string& command);
    
    // Get shared tool factory instance
    std::shared_ptr<ToolFactory> get_tool_factory();
}
