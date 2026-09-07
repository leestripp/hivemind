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

    // §FIX-global-state-races: these globals are read/written from tool
    // execution paths that can run on multiple threads (subagents,
    // ThreadPool). One mutex guards the approval sets and the cwd path;
    // the two flags are atomic so their simple load/store sites stay
    // lock-free.
    std::mutex g_state_mutex;

    // Session-level cache for approved commands (in-memory only)
    std::set<std::string> session_approved_commands;

    // Persisted approvals (loaded from .daedalus/approvals.json)
    std::set<std::string> persisted_approved_commands;

    // Current working directory tracker
    std::filesystem::path current_working_directory;
    std::atomic<bool> cwd_initialized{false};

    // Auto mode - automatically approve tool executions
    std::atomic<bool> auto_mode_enabled{false};

    // §BUG-008 — when false, BashTool does not record host-approved
    // commands into the session-approved cache (hosts that want
    // "ask every time" semantics). Default true = legacy behaviour.
    std::atomic<bool> remember_approvals{true};

    // §Phase 3 — sink set by LLMClient::set_sink(). Tools use this for
    // blocking I/O like execute_bash approval. weak_ptr because the host
    // owns the sink's lifetime.
    std::weak_ptr<hive::AgentSink> g_sink;

    // Path to the approvals JSON file. Lives in the per-agent project
    // tier (§Phase 16: new layout is .daedalus/hive-brain[-<name>]/).
    std::filesystem::path get_approvals_file_path(
        const std::filesystem::path& project_path,
        std::string_view agent_name = {}) {
        return hive::resolve_agent_dir(project_path, agent_name)
               / "approvals.json";
    }

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

    // Load persisted approvals from JSON file
    void load_persisted_approvals(const std::filesystem::path& project_path,
                                  std::string_view agent_name) {
        std::filesystem::path approvals_file = get_approvals_file_path(project_path, agent_name);
        if (std::filesystem::exists(approvals_file)) {
            try {
                std::ifstream infile(approvals_file);
                json data = json::parse(infile);
                infile.close();
                if (data.contains("approved_commands") && data["approved_commands"].is_array()) {
                    std::lock_guard<std::mutex> lock(g_state_mutex);
                    for (const auto& cmd : data["approved_commands"]) {
                        persisted_approved_commands.insert(cmd.get<std::string>());
                    }
                }
            } catch (const std::exception& e) {
                LOG_CERR(WARNING, "[WARNING] Failed to load persisted approvals: " << e.what());
            }
        }
    }

    // Save persisted approvals to JSON file
    void save_persisted_approvals(const std::filesystem::path& project_path,
                                  std::string_view agent_name) {
        std::filesystem::path approvals_file = get_approvals_file_path(project_path, agent_name);
        try {
            std::filesystem::path state_dir = approvals_file.parent_path();
            if (!std::filesystem::exists(state_dir)) {
                std::filesystem::create_directories(state_dir);
            }
            json data;
            data["approved_commands"] = json::array();
            {
                std::lock_guard<std::mutex> lock(g_state_mutex);
                for (const auto& cmd : persisted_approved_commands) {
                    data["approved_commands"].push_back(cmd);
                }
            }
            std::ofstream outfile(approvals_file);
            outfile << data.dump(2);
            outfile.close();
        } catch (const std::exception& e) {
            LOG_CERR(ERROR, "[APPROVALS] Failed to save approvals: " << e.what());
        }
    }

    // Clear the session-approved commands cache
    void clear_session_approved_commands() {
        std::lock_guard<std::mutex> lock(g_state_mutex);
        session_approved_commands.clear();
    }

    // Add a command to session approved commands (internal use by BashTool)
    void add_session_approved_command_internal(const std::string& command) {
        std::lock_guard<std::mutex> lock(g_state_mutex);
        session_approved_commands.insert(command);
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

    // Set auto mode (automatically approve tool executions)
    void set_auto_mode(bool enabled) {
        auto_mode_enabled = enabled;
    }

    // Check if auto mode is enabled
    bool is_auto_mode_enabled() {
        return auto_mode_enabled.load();
    }

    // §BUG-008 — remember-approvals switch
    void set_remember_approvals(bool enabled) {
        remember_approvals = enabled;
    }

    bool remember_approvals_enabled() {
        return remember_approvals.load();
    }

    // §Phase 3 — sink storage. Tools (e.g. execute_bash approval) use this
    // to call back into the host. weak_ptr because the sink lifetime is
    // owned by the host (LLMClient keeps a shared_ptr internally).

    void set_sink(std::shared_ptr<hive::AgentSink> sink) {
        g_sink = sink;
    }

    // §BUG-008 — report which cache approved a command (or nullopt when no
    // cache matched). Same matching rules as is_command_session_approved().
    std::optional<hive::ApprovalSource> approval_cache_source(
        const std::string& command) {
        std::lock_guard<std::mutex> lock(g_state_mutex);
        for (const auto& approved : session_approved_commands) {
            if (matches_pattern(command, approved)) {
                return hive::ApprovalSource::SessionCache;
            }
        }
        for (const auto& approved : persisted_approved_commands) {
            if (matches_pattern(command, approved)) {
                return hive::ApprovalSource::PersistedCache;
            }
        }
        return std::nullopt;
    }

    // §BUG-008 — notify the host that a command was approved without a
    // prompt. Best-effort: a throwing sink must not block execution.
    void notify_auto_approved(const std::string& command,
                              hive::ApprovalSource source) {
        if (auto s = g_sink.lock()) {
            try {
                s->on_command_auto_approved(command, source);
            } catch (...) { /* informational only */ }
        }
    }

    bool request_command_approval(const std::string& command) {
        // Auto mode always approves.
        if (auto_mode_enabled) {
            notify_auto_approved(command, hive::ApprovalSource::AutoApprove);
            return true;
        }

        // If the command is already in the session or persisted approved
        // set, approve without prompting. (Backward-compatible behaviour.)
        // §BUG-008: the host is told WHICH cache bypassed its prompt.
        if (auto source = approval_cache_source(command)) {
            notify_auto_approved(command, *source);
            return true;
        }

        // Otherwise, ask the host via the sink.
        if (auto s = g_sink.lock()) {
            try {
                return s->request_command_approval(command);
            } catch (...) {
                // Host threw — abort the approval.
                return false;
            }
        }

        // No sink attached and not auto-mode → deny (preserves the legacy
        // "banned by default" semantics that were the default before Phase 3).
        return false;
    }

    // Clear persisted approvals file
    void clear_persisted_approvals(const std::filesystem::path& project_path,
                                   std::string_view agent_name) {
        {
            std::lock_guard<std::mutex> lock(g_state_mutex);
            persisted_approved_commands.clear();
        }
        std::filesystem::path approvals_file = get_approvals_file_path(project_path, agent_name);
        if (std::filesystem::exists(approvals_file)) {
            std::filesystem::remove(approvals_file);
        }
        std::filesystem::path state_dir = approvals_file.parent_path();
        if (std::filesystem::exists(state_dir) && std::filesystem::is_empty(state_dir)) {
            std::filesystem::remove(state_dir);
        }
    }

    // Add a command to persisted approvals and save to file
    void add_persisted_approval(const std::filesystem::path& project_path,
                                std::string_view agent_name,
                                const std::string& command) {
        // Add to persisted set
        {
            std::lock_guard<std::mutex> lock(g_state_mutex);
            persisted_approved_commands.insert(command);
        }
        // Save to file
        save_persisted_approvals(project_path, agent_name);
    }

    // Check if a command matches a pattern (supports multiple wildcards for files)
    bool matches_pattern(const std::string& command, const std::string& pattern) {
        // Exact match
        if (command == pattern) {
            return true;
        }

        // No wildcards — prefix match (original behavior)
        if (pattern.find('*') == std::string::npos) {
            return command.compare(0, pattern.size(), pattern) == 0;
        }

        // Split pattern by '*' to get the literal parts
        std::vector<std::string> parts;
        std::string current;
        for (char c : pattern) {
            if (c == '*') {
                if (!current.empty()) {
                    parts.push_back(current);
                }
                current.clear();
            } else {
                current += c;
            }
        }
        if (!current.empty()) {
            parts.push_back(current);
        }

        // §FIX-leading-wildcard: a pure-"*" pattern (no literal parts)
        // matches everything. A leading '*' must NOT anchor the first
        // part as a prefix — "*.txt" is a suffix match and "*foo*" is
        // a contains match.
        if (parts.empty()) {
            return true;
        }

        const bool anchored_start = (pattern.front() != '*');
        const bool anchored_end   = (pattern.back() != '*');

        size_t search_start = 0;
        size_t part_begin = 0;

        // Pattern is like "cmd *" — command must start with the prefix.
        if (anchored_start) {
            if (command.size() < parts[0].size() ||
                command.compare(0, parts[0].size(), parts[0]) != 0) {
                return false;
            }
            search_start = parts[0].size();
            part_begin = 1;
        }

        // Middle parts (everything except a possibly-anchored suffix)
        // must appear in order.
        size_t part_end = parts.size();
        if (anchored_end) part_end -= 1;
        for (size_t i = part_begin; i < part_end; i++) {
            size_t pos = command.find(parts[i], search_start);
            if (pos == std::string::npos) {
                return false;
            }
            search_start = pos + parts[i].size();
        }

        // Pattern does not end with '*' — command must end with the
        // last part, at or after the position reached so far.
        if (anchored_end) {
            const std::string& suffix = parts.back();
            if (command.size() < suffix.size() ||
                command.size() - suffix.size() < search_start ||
                command.compare(command.size() - suffix.size(), suffix.size(), suffix) != 0) {
                return false;
            }
        }

        return true;
    }

    // List of safe read-only diagnostic tools
    const std::vector<std::string> safe_diagnostic_prefixes = {
        "node --check", "node -c",
        "python -m py_compile", "python -m pyflakes", "python -m mypy",
        "python -m pylint", "python -m flake8", "python -m ruff",
        "python -m black --check", "python -m isort --check-only",
        "python -m pytest --collect-only", "python -m pytest --dry-run",
        "rustc --check", "cargo check", "cargo clippy",
        "cargo test --no-run", "cargo build --dry-run",
        "g++ -fsyntax-only", "gcc -fsyntax-only",
        "clang++ -fsyntax-only", "clang -fsyntax-only",
        "cmake -N", "cmake --build . --target help",
        "go build -n", "go vet", "go test -c", "go mod verify",
        "npx tsc --noEmit", "npx eslint --no-eslintrc --quiet",
        "ruby -c", "perl -c", "shellcheck",
    };

    // Check if a command contains command chaining operators that require splitting

    // Check if a command matches any approved session or persisted command (supports wildcards)
    bool is_command_session_approved(const std::string& command) {
        // Note: Chained commands are handled specially in BashTool::execute
        // This function checks individual commands only
        return approval_cache_source(command).has_value();
    }

    // Escape shell argument

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
