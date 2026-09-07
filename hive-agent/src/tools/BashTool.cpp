#include "tools/BashTool.h"
#include "tools/ToolHandler.h"
#include "tools/BashCommandExecutor.h"
#include "tools/BashPathChecker.h"
#include "core/FileIO.h"
#include "core/BackgroundProcessManager.h"
#include "core/utils.h"

#include <sstream>
#include <vector>
#include <algorithm>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

// Build event callback implementation
BuildEventCallback BashTool::build_callback_ = nullptr;

// Trigger build callback if command looks like a build command
static void trigger_build_callback(const std::string& command, int exit_code) {
    auto callback = BashTool::get_build_callback();
    if (!callback) return;

    std::vector<std::string> build_patterns = {
        "cmake", "make", "g++", "gcc", "clang++", "clang",
        "build", "compile", "ninja",
        "npm build", "npm install", "npm test",
        "cargo build", "cargo test", "cargo run",
        "go build", "go test",
        "python -m pytest", "pytest", "ctest"
    };

    std::string cmd_lower = command;
    std::transform(cmd_lower.begin(), cmd_lower.end(), cmd_lower.begin(), ::tolower);

    for (const auto& pattern : build_patterns) {
        if (cmd_lower.find(pattern) != std::string::npos) {
            callback(command, exit_code == 0);
            return;
        }
    }
}

// ========== BashTool ==========

json BashTool::get_definition() const {
    return json::object({
        {"type", "function"},
        {"function", json::object({
            {"name", get_name()},
            {"description", get_description()},
            {"parameters", json::object({
                {"type", "object"},
                {"properties", json::object({
                    {"command", json::object({
                        {"type", "string"},
                        {"description", "The shell command to execute. Required for run/start. A trailing unquoted & is interpreted as a request to run in the background."}
                    })},
                    {"timeout", json::object({
                        {"type", "integer"},
                        {"description", "Optional timeout in seconds for synchronous commands. Default: 60. Range: 1-3600."}
                    })},
                    {"background", json::object({
                        {"type", "boolean"},
                        {"description", "Shorthand for action=start. Start the command detached and return a process_id."}
                    })},
                    {"action", json::object({
                        {"type", "string"},
                        {"description", "Execution mode: run (default), start, poll, send, stop."},
                        {"enum", json::array({"run", "start", "poll", "send", "stop"})}
                    })},
                    {"process_id", json::object({
                        {"type", "string"},
                        {"description", "Background process ID returned by action=start. Required for poll/send/stop."}
                    })},
                    {"input", json::object({
                        {"type", "string"},
                        {"description", "Data to send to a background process's stdin. Required for action=send."}
                    })},
                    {"clear_output", json::object({
                        {"type", "boolean"},
                        {"description", "If true, clear the output buffer after reading (action=poll only)."}
                    })}
                })},
                {"required", json::array()}
            })}
        })}
    });
}

json BashTool::execute(const json& args, FileIO& file_io) {
    return execute(args, file_io, nullptr);
}

json BashTool::execute(const json& args, FileIO& file_io, ToolProgressFn progress) {
    auto emit_progress = [&progress](const std::string& stage,
                                     int percent,
                                     const std::string& message) {
        if (!progress) return;
        json p = {
            {"stage",   stage},
            {"percent", percent},
            {"message", message},
        };
        progress(p);
    };

    try {
        // ===== Parse Arguments =====
        std::string command;
        int timeout_seconds = get_default_timeout_seconds();
        Action action = Action::Run;
        std::string process_id;
        std::string input;
        bool clear_output = false;

        const json& obj = args.is_object() ? args : json::parse(args.get<std::string>());

        if (obj.contains("action")) {
            action = parse_action(obj["action"].get<std::string>());
        }
        if (obj.contains("background") && obj["background"].get<bool>()) {
            action = Action::Start;
        }
        if (obj.contains("command")) {
            command = obj["command"].get<std::string>();
        }
        if (obj.contains("timeout")) {
            timeout_seconds = obj["timeout"].get<int>();
            if (timeout_seconds < get_min_timeout_seconds() ||
                timeout_seconds > get_max_timeout_seconds()) {
                return json::object({
                    {"status", "error"},
                    {"message", "Timeout must be between " + std::to_string(get_min_timeout_seconds()) +
                                " and " + std::to_string(get_max_timeout_seconds()) + " seconds."}
                });
            }
        }
        if (obj.contains("process_id")) {
            process_id = obj["process_id"].get<std::string>();
        }
        if (obj.contains("input")) {
            input = obj["input"].get<std::string>();
        }
        if (obj.contains("clear_output")) {
            clear_output = obj["clear_output"].get<bool>();
        }

        // Auto-detect trailing background operator and switch to start mode.
        if (action == Action::Run && !command.empty() && is_background_command(command)) {
            action = Action::Start;
            command = strip_background_ampersand(command);
        }

        const std::string project_path = file_io.get_project_path().string();

        // ===== Security / approval (only for run and start) =====
        if (action == Action::Run || action == Action::Start) {
            if (command.empty()) {
                return json::object({{"status", "error"}, {"message", "Missing or empty 'command' argument"}});
            }

            // Block privilege escalation
            if (BashPathChecker::contains_privilege_escalation(command) && !ToolHandler::is_auto_mode_enabled()) {
                return json::object({
                    {"status", "error"},
                    {"message", BashPathChecker::privilege_escalation_message(command)}
                });
            }

            // Ask host for approval
            if (!ToolHandler::request_command_approval(command)) {
                return json::object({
                    {"status", "denied"},
                    {"message", "Command denied by host."}
                });
            }
            // §BUG-008: hosts can opt out of auto-recording approvals
            // ("ask every time" semantics) via set_remember_approvals(false).
            if (ToolHandler::remember_approvals_enabled()) {
                ToolHandler::add_session_approved_command_internal(command);
            }
        }

        // ===== Dispatch =====
        switch (action) {
        case Action::Run:
            emit_progress("starting", 0,
                          "Starting command: " + command.substr(0, std::min<size_t>(80, command.size())));
            emit_progress("executing", 50, "Running command...");
            return run_sync(command, project_path, timeout_seconds, progress);

        case Action::Start:
            return run_background(command, project_path, process_id);

        case Action::Poll:
            if (process_id.empty()) {
                return json::object({{"status", "error"}, {"message", "Missing 'process_id' for poll action"}});
            }
            return poll_background(process_id, clear_output);

        case Action::Send:
            if (process_id.empty()) {
                return json::object({{"status", "error"}, {"message", "Missing 'process_id' for send action"}});
            }
            return send_background(process_id, input);

        case Action::Stop:
            if (process_id.empty()) {
                return json::object({{"status", "error"}, {"message", "Missing 'process_id' for stop action"}});
            }
            return stop_background(process_id);
        }

        return json::object({{"status", "error"}, {"message", "Unknown execute_bash action"}});

    } catch (const std::exception& e) {
        return json::object({{"status", "error"}, {"message", std::string("Error executing bash tool: ") + e.what()}});
    }
}

BashTool::Action BashTool::parse_action(const std::string& str) {
    std::string s = str;
    std::transform(s.begin(), s.end(), s.begin(), ::tolower);
    if (s == "start") return Action::Start;
    if (s == "poll") return Action::Poll;
    if (s == "send") return Action::Send;
    if (s == "stop") return Action::Stop;
    return Action::Run;
}

bool BashTool::is_background_command(const std::string& command) {
    return BashPathChecker::contains_standalone_ampersand(command);
}

std::string BashTool::strip_background_ampersand(const std::string& command) {
    std::string trimmed = command;
    // Trim trailing whitespace, then remove trailing & if present.
    size_t end = trimmed.find_last_not_of(" \t\n\r");
    if (end != std::string::npos) {
        trimmed.erase(end + 1);
    } else {
        trimmed.clear();
    }
    if (!trimmed.empty() && trimmed.back() == '&') {
        trimmed.pop_back();
        // Trim again after removing &
        end = trimmed.find_last_not_of(" \t\n\r");
        if (end != std::string::npos) {
            trimmed.erase(end + 1);
        } else {
            trimmed.clear();
        }
    }
    return trimmed;
}

json BashTool::run_sync(const std::string& command,
                        const std::string& project_path,
                        int timeout_seconds,
                        ToolProgressFn progress) {
    // Forward live output chunks through the progress callback so the host
    // can display sanitised command output while the command is still running.
    auto chunk_cb = [&progress](std::string_view chunk) {
        if (!progress) return;
        json p = {
            {"stage", "output"},
            {"output_chunk", std::string(chunk)},
        };
        progress(p);
    };

    auto result = BashCommandExecutor::execute(
        command, project_path, timeout_seconds, std::move(chunk_cb));
    trigger_build_callback(command, result.exit_code);

    return json::object({
        {"status", result.success ? "success" : "error"},
        {"command", command},
        {"output", result.output},
        {"exit_code", result.exit_code}
    });
}

json BashTool::run_background(const std::string& command,
                              const std::string& project_path,
                              const std::string& requested_id) {
    auto& manager = BackgroundProcessManager::get_instance();

    // BackgroundProcessManager assigns its own process IDs. If the caller
    // supplied one we ignore it for now; the returned ID is authoritative.
    (void)requested_id;

    std::string process_id = manager.start_background(command, project_path);
    if (process_id.empty()) {
        return json::object({
            {"status", "error"},
            {"message", "Failed to start background process: " + command}
        });
    }

    auto info = manager.get_process_info(process_id);
    if (!info) {
        return json::object({
            {"status", "error"},
            {"message", "Process started but info not found"}
        });
    }

    return json::object({
        {"status", "background"},
        {"process_id", process_id},
        {"pid", static_cast<int>(info->pid)},
        {"command", command},
        {"message", "Process started in background. Use action=poll with process_id to read output, action=send to write stdin, action=stop to terminate."}
    });
}

json BashTool::poll_background(const std::string& process_id, bool clear_output) {
    auto& manager = BackgroundProcessManager::get_instance();

    auto info = manager.get_process_info(process_id);
    if (!info) {
        return json::object({
            {"status", "error"},
            {"message", "Process not found: " + process_id}
        });
    }

    auto output = manager.poll_output(process_id, clear_output);

    // Strip terminal-control sequences (cursor movement, screen erase, etc.)
    // while preserving UTF-8 text and ANSI SGR colour/style codes. This keeps
    // background process output safe to forward to the API/host TTY.
    output.stdout = BashCommandExecutor::sanitize_tty_sequences(output.stdout);
    output.stderr = BashCommandExecutor::sanitize_tty_sequences(output.stderr);

    json result = json::object({
        {"status", "success"},
        {"process_id", process_id},
        {"exited", output.exited}
    });

    if (output.exited) {
        result["exit_code"] = output.exit_code;
        if (output.signaled) {
            result["signaled"] = true;
            result["signal"] = output.signal_num;
        }
    }

    result["stdout"] = output.stdout;
    result["stderr"] = output.stderr;

    if (!output.stdout.empty() || !output.stderr.empty()) {
        result["message"] = "Output from process (stdout: " +
            std::to_string(output.stdout.size()) + " bytes, stderr: " +
            std::to_string(output.stderr.size()) + " bytes)";
    } else if (output.exited) {
        result["message"] = "Process has exited with code " + std::to_string(output.exit_code);
    } else {
        result["message"] = "No output available yet";
    }

    return result;
}

json BashTool::send_background(const std::string& process_id, const std::string& input) {
    auto& manager = BackgroundProcessManager::get_instance();

    auto info = manager.get_process_info(process_id);
    if (!info) {
        return json::object({
            {"status", "error"},
            {"message", "Process not found: " + process_id}
        });
    }

    if (manager.has_exited(process_id)) {
        return json::object({
            {"status", "error"},
            {"process_id", process_id},
            {"exited", true},
            {"message", "Process has already exited"}
        });
    }

    auto result = manager.send_to_process(process_id, input);
    if (!result.success) {
        return json::object({
            {"status", "error"},
            {"process_id", process_id},
            {"message", result.error}
        });
    }

    return json::object({
        {"status", "success"},
        {"process_id", process_id},
        {"bytes_sent", result.bytes_sent}
    });
}

json BashTool::stop_background(const std::string& process_id) {
    auto& manager = BackgroundProcessManager::get_instance();

    auto info = manager.get_process_info(process_id);
    if (!info) {
        return json::object({
            {"status", "error"},
            {"message", "Process not found: " + process_id}
        });
    }

    bool success = manager.stop_process(process_id);
    if (!success) {
        return json::object({
            {"status", "error"},
            {"process_id", process_id},
            {"message", "Failed to stop process"}
        });
    }

    return json::object({
        {"status", "success"},
        {"process_id", process_id},
        {"message", "Process stopped and resources cleaned up"}
    });
}

// Safe diagnostic prefixes for syntax checking (allowed without path restrictions)
const std::vector<std::string>& BashTool::get_safe_diagnostic_prefixes() {
    static const std::vector<std::string> prefixes = {
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
    return prefixes;
}
