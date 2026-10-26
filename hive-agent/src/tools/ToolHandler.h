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

    // Set the sink that receives tool lifecycle events.
    void set_sink(std::shared_ptr<hive::AgentSink> sink);

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
    
    // Current working directory management
    void init_current_working_directory(const std::filesystem::path& project_root);
    std::filesystem::path get_current_working_directory();
    bool is_cwd_initialized();
    
    // Get shared tool factory instance
    std::shared_ptr<ToolFactory> get_tool_factory();
}
