#pragma once

#include <string>
#include <vector>
#include <functional>
#include <nlohmann/json.hpp>
#include "BaseTool.h"
#include "BashPathChecker.h"

class FileIO;

using json = nlohmann::json;

/**
 * BashTool.h - Shell command execution tool
 *
 * Supports two execution models:
 *   - Synchronous (default): run a command, capture output, return exit code.
 *   - Background: start a detached process, poll its output, send stdin, stop it.
 *
 * Background commands are delegated to BackgroundProcessManager so the agent
 * worker thread is never blocked waiting on a server or long-running tool.
 */

// Build event callback type
using BuildEventCallback = std::function<void(const std::string& command, bool success)>;

// ========== BashTool ==========
class BashTool : public BaseTool {
public:
    enum class Action {
        Run,     // synchronous execution (default)
        Start,   // start a background process
        Poll,    // read background process output
        Send,    // send stdin to a background process
        Stop     // stop a background process
    };

    [[nodiscard]] std::string get_name() const override { return "execute_bash"; }
    [[nodiscard]] std::string get_description() const override {
        return "Execute shell commands in the project root. Supports synchronous execution and background/detached processes for servers or long-running tools. IMPORTANT: a synchronous run blocks your ENTIRE agent until the command exits or the timeout fires — you cannot answer messages while it runs. Servers, daemons, watchers, and anything long-running MUST be started in the background (background:true, action:start, or a trailing &) and monitored with action=poll; never run them synchronously.";
    }
    [[nodiscard]] json get_definition() const override;
    json execute(const json& args, FileIO& file_io) override;
    json execute(const json& args, FileIO& file_io, ToolProgressFn progress) override;

    // Set callback for build events (called from MiniMaxClient)
    static void set_build_callback(BuildEventCallback callback) { build_callback_ = callback; }

    // Get callback for use in helper functions
    static BuildEventCallback get_build_callback() { return build_callback_; }

    // ====== Public Helper Functions for Testing ======
    static std::string extract_path_from_command(const std::string& command) {
        return BashPathChecker::extract_path_from_command(command);
    }
    static bool contains_standalone_ampersand(const std::string& command) {
        return BashPathChecker::contains_standalone_ampersand(command);
    }

    // Get default timeout in seconds (60 seconds)
    static constexpr int get_default_timeout_seconds() { return 60; }

    // Get min/max allowed timeout values
    static constexpr int get_min_timeout_seconds() { return 1; }
    static constexpr int get_max_timeout_seconds() { return 3600; }  // 1 hour max

private:
    static Action parse_action(const std::string& str);
    static bool is_background_command(const std::string& command);
    static std::string strip_background_ampersand(const std::string& command);

    static json run_sync(const std::string& command,
                         const std::string& project_path,
                         int timeout_seconds,
                         ToolProgressFn progress);

    static json run_background(const std::string& command,
                               const std::string& project_path,
                               const std::string& requested_id);

    static json poll_background(const std::string& process_id, bool clear_output);
    static json send_background(const std::string& process_id, const std::string& input);
    static json stop_background(const std::string& process_id);

    // Static callback for build events
    static BuildEventCallback build_callback_;
};
