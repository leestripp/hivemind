#include "tools/ToolHandler.h"
#include "tools/ToolFactory.h"
#include "tools/MemoryTool.h"
#include "core/FileIO.h"
#include "core/SessionMemory.h"
#include "core/CommonUtils.h"
#include "core/Logger.h"
#include "library/default_paths.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <stdexcept>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <set>
#include <cctype>
#include <atomic>
#include <mutex>
#include <optional>


// §Phase 5.1: helper macro that converts LOG_CERR(INFO,  ...); style
// statements into LOG_INFO/LOG_WARNING/LOG_ERROR calls via the
// existing Logger. Same formatted text routed through the Logger
// instead of writing to stderr directly.
#define LOG_CERR(level, expr) do {                          \
        std::ostringstream _hive_msg;                   \
        _hive_msg << expr;                               \
        LOG_##level(_hive_msg.str());                    \
    } while (0)
namespace ToolHandler {
    std::shared_ptr<ToolFactory> g_tool_factory;

    // Tool execution can run on multiple threads (subagents/ThreadPool).
    std::mutex g_state_mutex;
    std::weak_ptr<hive::AgentSink> g_sink;

    // Current working directory tracker
    std::filesystem::path current_working_directory;
    std::atomic<bool> cwd_initialized{false};

    // Initialize the tool factory
    void init_tool_factory() {
        if (!g_tool_factory) {
            g_tool_factory = std::make_shared<ToolFactory>();
        }
    }

    // Get the tool factory instance
    std::shared_ptr<ToolFactory> get_tool_factory() {
        if (!g_tool_factory) {
            init_tool_factory();
        }
        return g_tool_factory;
    }
    
    // Initialize memory tools with SessionMemory pointer
    void init_memory_tools(SessionMemory* session_memory) {
        RememberTool::set_session_memory(session_memory);
        ForgetTool::set_session_memory(session_memory);
        MemoriesTool::set_session_memory(session_memory);
        MemoryStatsTool::set_session_memory(session_memory);
    }

    void set_sink(std::shared_ptr<hive::AgentSink> sink) {
        g_sink = std::move(sink);
    }

    // Initialize current working directory with project root
    void init_current_working_directory(const std::filesystem::path& project_root) {
        {
            std::lock_guard<std::mutex> lock(g_state_mutex);
            current_working_directory = project_root;
        }
        cwd_initialized = true;
    }

    // Get current working directory (must be initialized first)
    std::filesystem::path get_current_working_directory() {
        std::lock_guard<std::mutex> lock(g_state_mutex);
        return current_working_directory;
    }

    // Check if cwd has been initialized
    bool is_cwd_initialized() {
        return cwd_initialized.load();
    }

    // Execute a tool call - delegates to appropriate tool
    json execute_tool_call(const json& tool_call, FileIO& file_io) {
        return execute_tool_call(tool_call, file_io, nullptr);
    }

    json execute_tool_call(const json& tool_call,
                           FileIO& file_io,
                           ToolProgressFn progress) {
        // Initialize tool factory if not done
        init_tool_factory();
        
        std::string tool_name;
        json args;
        
        try {
            if (tool_call.is_object()) {
                // Support both formats:
                // 1. {"function": {"name": "tool_name", "arguments": "{\"param\": \"value\"}"}} (from LLM - arguments is STRING!)
                // 2. {"function": {"name": "tool_name", "arguments": {"param": "value"}}} (parsed format)
                // 3. {"name": "tool_name", "arguments": {...}} (legacy format)
                if (tool_call.contains("function") && tool_call["function"].is_object()) {
                    tool_name = tool_call["function"].value("name", "");
                    
                    // Get arguments - could be a string or object
                    auto& func_obj = tool_call["function"];
                    if (func_obj.contains("arguments")) {
                        auto& arg_val = func_obj["arguments"];
                        if (arg_val.is_string()) {
                            // Arguments is a JSON string - parse it!
                            std::string args_str = arg_val.get<std::string>();
                            try {
                                args = json::parse(args_str);
                            } catch (const json::parse_error& e) {
                                std::string err_msg = "[ERROR] Failed to parse arguments JSON string: " + std::string(e.what());
                                LOG_CERR(INFO, err_msg);
                                LOG_ERROR("ToolHandler: " + err_msg);
                                
                                // Try to recover truncated JSON by completing unclosed braces
                                std::string recovered = hive::try_recover_json_string(args_str);
                                bool recovery_succeeded = false;
                                if (recovered != args_str) {
                                    // Recovery attempted, try parsing again
                                    try {
                                        args = json::parse(recovered);
                                        LOG_CERR(INFO, "[INFO] Successfully recovered JSON arguments");
                                        recovery_succeeded = true;
                                    } catch (...) {
                                        // Recovery didn't work, continue to error handling
                                        LOG_CERR(INFO, "[INFO] JSON recovery failed, using escaped raw args");
                                    }
                                }
                                
                                // If recovery succeeded, skip the error return
                                if (recovery_succeeded) {
                                    // args is now set to the recovered JSON, continue with tool execution
                                } else {
                                    // Escape problematic characters in the raw arguments to prevent
                                    // JSON serialization issues. The original string may contain
                                    // unescaped quotes or special characters that break JSON parsing.
                                    std::string escaped_args = args_str;
                                    for (size_t i = 0; i < escaped_args.size(); i++) {
                                        char c = escaped_args[i];
                                        if (c == '"' || c == '\\' || c < 0x20) {
                                            escaped_args[i] = '?';
                                        }
                                    }
                                    
                                    return json::object({
                                        {"status", "error"},
                                        {"message", "Invalid arguments JSON - check raw_arguments for details"},
                                        {"error_type", "parse_error"},
                                        {"error_details", std::string(e.what())},
                                        {"raw_arguments", escaped_args}
                                    });
                                }
                            }
                        } else {
                            args = arg_val;
                        }
                    } else {
                        args = json::object();
                    }
                } else {
                    tool_name = tool_call.value("name", "");
                    args = tool_call.value("arguments", json::object());
                }
            } else {
                LOG_ERROR("Tool call is not an object: " + tool_call.dump());
                return json::object({{"status", "error"}, {"message", "Invalid tool call format"}});
            }
            
            if (tool_name.empty()) {
                LOG_ERROR("Empty tool name in tool call");
                return json::object({{"status", "error"}, {"message", "Missing tool name"}});
            }
        } catch (const std::exception& e) {
            LOG_ERROR("Exception parsing tool call: " + std::string(e.what()));
            return json::object({{"status", "error"}, {"message", std::string("Error parsing tool call: ") + e.what()}});
        }
        
        // Delegate to tool factory for all tools (including execute_bash)
        return g_tool_factory->execute_tool(tool_name, args, file_io, progress);
    }

    json execute_tool_call(const json& tool_call,
                           FileIO& file_io,
                           ToolProgressFn progress,
                           const ToolFilter& filter) {
        // Parse the call, then execute with the filter.
        std::string tool_name;
        json args;

        if (tool_call.is_object()) {
            if (tool_call.contains("function") && tool_call["function"].is_object()) {
                tool_name = tool_call["function"].value("name", "");
            } else {
                tool_name = tool_call.value("name", "");
            }
        }

        if (!filter.is_tool_allowed(tool_name)) {
            return json::object({
                {"status", "error"},
                {"message", "Tool '" + tool_name + "' is disabled by host policy"}
            });
        }

        return execute_tool_call(tool_call, file_io, progress);
    }

    // Get tool definitions as JSON string
    std::string get_tool_definitions() {
        // Initialize tool factory if not done
        init_tool_factory();
        
        json definitions = g_tool_factory->get_all_definitions();
        return definitions.dump();
    }

    // Get tool definitions filtered by a per-agent ToolFilter
    std::string get_tool_definitions_filtered(const ToolFilter& filter) {
        init_tool_factory();

        json definitions = g_tool_factory->get_all_definitions(filter);
        return definitions.dump();
    }
    
    // Get tool definitions excluding specified tools (for sub-agent mode)
    std::string get_tool_definitions_filtered(const std::vector<std::string>& exclude_tools) {
        // Initialize tool factory if not done
        init_tool_factory();
        
        json definitions = g_tool_factory->get_all_definitions();
        
        // Filter out excluded tools
        json filtered = json::array();
        for (const auto& def : definitions) {
            if (def.contains("function") && def["function"].contains("name")) {
                std::string tool_name = def["function"]["name"];
                bool excluded = false;
                for (const auto& excluded_name : exclude_tools) {
                    if (tool_name == excluded_name) {
                        excluded = true;
                        break;
                    }
                }
                if (!excluded) {
                    filtered.push_back(def);
                }
            }
        }
        return filtered.dump();
    }
}
