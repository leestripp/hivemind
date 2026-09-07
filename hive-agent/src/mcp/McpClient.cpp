#include "McpClient.h"
#include "core/ThreadPool.h"
#include <iostream>
#include <cstring>
#include <unistd.h>
#include <chrono>
#include <poll.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <optional>
#include <vector>
#include <string>
#include <map>
#include <functional>
#include <thread>
#include <nlohmann/json.hpp>

#include "core/Logger.h"

// §Phase 5.1: helper macro that converts LOG_CERR(INFO,  ...); style
// statements into LOG_INFO/LOG_WARNING/LOG_ERROR calls via the
// existing Logger. Same formatted text routed through the Logger
// instead of writing to stderr directly.
#define LOG_CERR(level, expr) do {                          \
        std::ostringstream _daedalus_msg;                   \
        _daedalus_msg << expr;                               \
        LOG_##level(_daedalus_msg.str());                    \
    } while (0)
namespace mcp {

namespace {

// §fix (2026-07-26): write(), not send() — these fds are pipes, and
// send() on a pipe fails with ENOTSOCK on Linux, so no stdio MCP server
// ever received a request. Regressed by the §fix (2026-07-18) change
// that switched write() → send(MSG_NOSIGNAL) to dodge SIGPIPE; we keep
// that SIGPIPE safety by blocking SIGPIPE in the calling thread for the
// duration of the write instead. A closed subprocess stdin then returns
// EPIPE instead of raising SIGPIPE that would kill the host process.
// Loops on short writes; returns false on error with errno preserved.
bool write_all(int fd, const std::string& data) {
    sigset_t mask, prev_mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGPIPE);
    pthread_sigmask(SIG_BLOCK, &mask, &prev_mask);

    size_t off = 0;
    bool ok = true;
    int saved_errno = 0;
    while (off < data.size()) {
        ssize_t n = write(fd, data.c_str() + off, data.size() - off);
        if (n < 0) {
            if (errno == EINTR) continue;
            saved_errno = errno;
            ok = false;
            break;
        }
        off += static_cast<size_t>(n);
    }

    // A SIGPIPE raised while blocked is pending on this thread; discard
    // it before restoring the mask so it can't fire later. Skip this if
    // the caller had SIGPIPE blocked already — their pending signals are
    // their own business.
    if (!sigismember(&prev_mask, SIGPIPE)) {
        sigset_t pending;
        sigpending(&pending);
        if (sigismember(&pending, SIGPIPE)) {
            const timespec ts{0, 0};
            sigtimedwait(&mask, nullptr, &ts);
        }
    }
    pthread_sigmask(SIG_SETMASK, &prev_mask, nullptr);

    if (!ok) errno = saved_errno;
    return ok;
}

} // namespace

McpClient::McpClient()
    : pipes_(nullptr)
    , connected_(false)
    , initialized_(false)
    , next_request_id_(1) {
}

McpClient::~McpClient() {
    disconnect();
}

bool McpClient::connect(const std::string& command,
                         const std::map<std::string, std::string>& env,
                         [[maybe_unused]] int timeout_ms) {
    if (connected_) {
        last_error_ = "Already connected";
        return false;
    }
    
    // Allocate pipes
    pipes_ = new McpPipes();
    
    int stdin_pipe[2];
    int stdout_pipe[2];
    
    if (pipe(stdin_pipe) == -1 || pipe(stdout_pipe) == -1) {
        last_error_ = "Failed to create pipes: " + std::string(strerror(errno));
        delete pipes_;
        pipes_ = nullptr;
        return false;
    }
    
    // Make stdout read end non-blocking
    int flags = fcntl(stdout_pipe[0], F_GETFL);
    fcntl(stdout_pipe[0], F_SETFL, flags | O_NONBLOCK);
    
    // Fork
    pid_t pid = fork();
    
    if (pid == -1) {
        close(stdin_pipe[0]);
        close(stdin_pipe[1]);
        close(stdout_pipe[0]);
        close(stdout_pipe[1]);
        last_error_ = "Failed to fork: " + std::string(strerror(errno));
        delete pipes_;
        pipes_ = nullptr;
        return false;
    }
    
    if (pid == 0) {
        // Child process
        close(stdin_pipe[1]);  // Close write end of stdin
        close(stdout_pipe[0]); // Close read end of stdout

        // §9.7.A-fix (2026-07-18): give the child its OWN process group.
        // Previously the child joined the AGENT's process group
        // (setpgid(0, getpgid(getppid()))), so disconnect()'s
        // killpg(getpgid(child_pid), SIGTERM) signalled the whole group —
        // including the hive-brain host itself. setpgid(0, 0) makes the
        // child a group leader (same pattern as src/core/Subprocess.cpp),
        // so killpg() only hits the server's own tree.
        if (setpgid(0, 0) != 0) {
            // Not fatal: disconnect() falls back to per-PID kill() when
            // the group lookup fails. Non-fatal: continue.
            (void)0;
        }

        // Redirect stdin
        if (dup2(stdin_pipe[0], STDIN_FILENO) == -1) {
            _exit(127);
        }
        // Redirect stdout
        if (dup2(stdout_pipe[1], STDOUT_FILENO) == -1) {
            _exit(127);
        }

        // §Demo fix: MCP server installers (uv/pip) print progress
        // spinners to stderr. Route stderr to /dev/null so the host
        // terminal stays clean and the CLI prompt isn't overwritten.
        int devnull = ::open("/dev/null", O_WRONLY);
        if (devnull >= 0) {
            ::dup2(devnull, STDERR_FILENO);
            ::close(devnull);
        }

        close(stdin_pipe[0]);
        close(stdout_pipe[1]);
        
        // Set up environment - explicitly set PATH so commands can be found
        // This is critical when .bashrc/.profile are missing or broken
        for (const auto& [key, value] : env) {
            if (setenv(key.c_str(), value.c_str(), 1) != 0) {
                // Log but continue
            }
        }
        
        // CRITICAL: Ensure PATH is set so uvx/commands can be found
        // If PATH is not set, use a default that includes ~/.local/bin (where uvx lives)
        char* current_path = getenv("PATH");
        if (current_path == nullptr || strlen(current_path) == 0) {
            setenv("PATH", "/home/user/.local/bin:/usr/local/bin:/usr/bin:/bin", 1);
        }
        
        // Parse command - inline parsing logic
        std::vector<std::string> args;
        std::string current;
        bool in_quote = false;
        char quote_char = '\0';
        
        for (size_t i = 0; i < command.size(); ++i) {
            char c = command[i];
            
            if (c == '"' || c == '\'') {
                if (in_quote) {
                    if (c == quote_char) {
                        in_quote = false;
                    } else {
                        current += c;
                    }
                } else {
                    in_quote = true;
                    quote_char = c;
                }
            } else if (c == ' ' || c == '\t') {
                if (!current.empty()) {
                    args.push_back(current);
                    current.clear();
                }
            } else {
                current += c;
            }
        }
        if (!current.empty()) {
            args.push_back(current);
        }
        
        // Build argv - wrap with bash -c to ensure proper shell environment
        std::vector<char*> argv;
        argv.push_back(const_cast<char*>("/bin/bash"));
        argv.push_back(const_cast<char*>("-c"));
        
        // Reconstruct the command string for bash -c. Single-quote EVERY
        // argument, escaping embedded single quotes as '\''. The previous
        // selective quoting (only spaces, '*', '?') let other shell
        // metacharacters through unquoted — e.g. the version spec
        // "mcp<2" was interpreted by bash as a stdin redirection from a
        // file named "2", which killed the server before it started
        // (§MCP-QUOTE).
        std::string full_command;
        for (size_t i = 0; i < args.size(); ++i) {
            if (i > 0) full_command += " ";
            full_command += '\'';
            for (const char ch : args[i]) {
                if (ch == '\'') full_command += "'\\''";
                else full_command += ch;
            }
            full_command += '\'';
        }
        argv.push_back(const_cast<char*>(full_command.c_str()));
        argv.push_back(nullptr);
        
        // Execute via bash to ensure proper shell environment
        execvp(argv[0], argv.data());
        _exit(127);
    }
    
    // Parent process
    close(stdin_pipe[0]);  // Close read end of stdin
    close(stdout_pipe[1]); // Close write end of stdout

    pipes_->stdin_write = stdin_pipe[1];
    pipes_->stdout_read = stdout_pipe[0];
    pipes_->child_pid = pid;

    // §9.7.A-fix (2026-07-18): no parent-side setpgid() — the child
    // puts itself in its own process group (setpgid(0, 0) above), so
    // killpg() in disconnect() can only ever hit the server's tree,
    // never the agent's own process group.
    //
    // §MCP-RACE: the child may not have completed setpgid() by the time
    // we return. Wait briefly until the child's process group differs
    // from ours before declaring the connection ready. This prevents a
    // very early disconnect() from calling killpg() on the host's group.
    {
        pid_t my_pgrp = getpgid(0);
        for (int i = 0; i < 50; ++i) {
            pid_t child_pgrp = getpgid(pid);
            if (child_pgrp > 0 && child_pgrp != my_pgrp) {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }

    connected_ = true;

    return true;
}

void McpClient::disconnect() {
    if (pipes_) {
        if (pipes_->child_pid > 0) {
            // §9.7.A — killpg first (covers all descendants), then
            // SIGKILL after a 2 s cap. The waitpid is non-blocking so
            // we don't hang on a zombie we can't reap.
            pid_t my_pgrp = getpgid(0);
            pid_t pgrp = getpgid(pipes_->child_pid);
            // §MCP-RACE: if the child has not yet moved to its own process
            // group, do NOT call killpg() — that would signal the host's
            // group. Fall back to killing the single child PID instead.
            if (pgrp > 0 && pgrp != my_pgrp) {
                killpg(pgrp, SIGTERM);
            } else {
                kill(pipes_->child_pid, SIGTERM);
            }
            int status = 0;
            // Wait up to 2 s for graceful shutdown.
            for (int i = 0; i < 20; ++i) {
                pid_t r = waitpid(pipes_->child_pid, &status, WNOHANG);
                if (r == pipes_->child_pid || r == -1) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            // If still alive, SIGKILL (again guarding the group path).
            if (kill(pipes_->child_pid, 0) == 0) {
                if (pgrp > 0 && pgrp != my_pgrp) killpg(pgrp, SIGKILL);
                else kill(pipes_->child_pid, SIGKILL);
                waitpid(pipes_->child_pid, &status, 0);
            }
        }
        delete pipes_;
        pipes_ = nullptr;
    }
    connected_ = false;
    initialized_ = false;
    tools_.clear();
}

bool McpClient::is_connected() const {
    return connected_;
}

bool McpClient::initialize() {
    if (!connected_) {
        last_error_ = "Not connected";
        return false;
    }

    if (initialized_) {
        return true;
    }

    // §SIGPIPE note (2026-07-14, updated 2026-07-26): the underlying
    // transport's send_request() / send_notification() write to the MCP
    // subprocess's stdin pipe via write() with SIGPIPE blocked in the
    // calling thread (see write_all()), which is the surgical fix for
    // "subprocess exits before parent writes its notification".
    // A closed pipe then returns EPIPE instead of raising SIGPIPE that
    // would kill the daemon. No signal-handler dance required.

    // Send initialize request
    json params = {
        {"protocolVersion", "2025-06-18"},
        {"capabilities", {{"roots", json::object()}, {"sampling", json::object()}}},
        {"clientInfo", {{"name", "hive-brain"}, {"version", "1.0.0"}}}
    };
    
    json response = send_request("initialize", params, 10000);
    
    if (response.is_null()) {
        return false;
    }
    
    if (response.contains("error")) {
        last_error_ = "Initialize error: " + response["error"].dump();
        return false;
    }
    
    // Store server info
    if (response.contains("result") && response["result"].contains("serverInfo")) {
        server_version_ = response["result"]["serverInfo"].value("version", "unknown");
    }
    
    initialized_ = true;
    
    // Send initialized notification (no response expected)
    send_notification("notifications/initialized", json::object());
    
    return true;
}

std::vector<ToolDefinition> McpClient::list_tools() {
    if (!initialized_) {
        last_error_ = "Not initialized";
        return {};
    }
    
    json response = send_request("tools/list", json::object());
    
    if (response.is_null() || response.contains("error")) {
        last_error_ = response.is_null() ? "No response" : response["error"].dump();
        return {};
    }
    
    std::vector<ToolDefinition> tools;
    
    if (response.contains("result") && response["result"].contains("tools")) {
        for (const auto& tool : response["result"]["tools"]) {
            ToolDefinition def;
            def.name = tool.value("name", "");
            def.description = tool.value("description", "");
            def.input_schema = tool.value("inputSchema", json::object());
            tools.push_back(def);
        }
    }
    
    tools_ = tools;
    return tools;
}

ToolResult McpClient::call_tool(const std::string& tool_name, const json& arguments) {
    if (!initialized_) {
        return {false, "", "Not initialized"};
    }
    
    json params = {
        {"name", tool_name},
        {"arguments", arguments}
    };
    
    json response = send_request("tools/call", params);
    
    if (response.is_null()) {
        return {false, "", last_error_};
    }
    
    if (response.contains("error")) {
        std::string error = response["error"].value("message", response["error"].dump());
        return {false, "", error};
    }
    
    // Parse result content
    std::string content;
    if (response.contains("result") && response["result"].contains("content")) {
        for (const auto& item : response["result"]["content"]) {
            if (item.contains("text")) {
                content += item["text"].get<std::string>();
            }
        }
    }
    
    return {true, content, std::nullopt};
}

std::string McpClient::understand_image(const std::string& image_url,
                                         const std::string& prompt) {
    json args = {
        {"prompt", prompt},
        {"image_source", image_url}  // Note: MiniMax MCP expects "image_source", not "image_url"
    };
    
    ToolResult result = call_tool("understand_image", args);
    
    if (result.success) {
        return result.content;
    }
    
    return "Error: " + (result.error.value_or("Unknown error"));
}

json McpClient::web_search(const std::string& query, [[maybe_unused]] int limit) {
    json args = {
        {"query", query}
    };

    ToolResult result = call_tool("web_search", args);

    if (result.success) {
        // Try to parse as JSON, otherwise return as text
        try {
            return json::parse(result.content);
        } catch (...) {
            return json{{"text", result.content}};
        }
    }

    return json{{"error", result.error.value_or("Unknown error")}};
}

// ============================================================================
// Async / batch support
// ============================================================================

std::future<ToolResult> McpClient::call_tool_async(
    const std::string& tool_name,
    const json& arguments,
    std::stop_token stop_token
) {
    // Submit the (synchronous) call_tool to the ThreadPool so it doesn't
    // block the caller. Cancellation is cooperative: if stop_token is
    // requested while the underlying read_response is polling, the
    // future will resolve once the in-flight request returns or times out.
    using hive::concurrent::ThreadPool;
    using hive::concurrent::TaskPriority;
    return ThreadPool::get_instance().submit(
        TaskPriority::NORMAL,
        [this, tool_name, arguments, stop_token]() {
            if (stop_token.stop_requested()) {
                return ToolResult{false, "", std::string("Cancelled")};
            }
            return call_tool(tool_name, arguments);
        }
    );
}

std::vector<std::future<ToolResult>> McpClient::call_tools_batch(
    const std::vector<ToolCallSpec>& calls
) {
    std::vector<std::future<ToolResult>> results;
    results.reserve(calls.size());

    // Each call is independent - submit them all to the pool in parallel.
    // For stdio transport the calls are serialized at the OS level (one pipe),
    // but at least they don't block the calling thread.
    using hive::concurrent::ThreadPool;
    using hive::concurrent::TaskPriority;
    for (const auto& call : calls) {
        results.push_back(
            ThreadPool::get_instance().submit(
                TaskPriority::NORMAL,
                [this, call]() {
                    return this->call_tool(call.tool_name, call.arguments);
                }
            )
        );
    }
    return results;
}

std::string McpClient::get_last_error() const {
    return last_error_;
}

// =============================================================================
// §9.7.C — list_changed notifications are now detected inline inside
// read_response() (search for "FIRING_LIST_CHANGED" in this file). The
// previous design — a separate listener thread that read from the same
// stdout pipe as the main reader — was broken: two threads racing for
// `read()` on a shared fd meant a real JSON-RPC response could be eaten
// by the listener and the caller would hang until timeout. The callbacks
// are now wired via set_list_changed_callback() / clear_list_changed_callback().
// =============================================================================

json McpClient::send_request(const std::string& method, const json& params, int timeout_ms) {
    if (!pipes_ || pipes_->stdin_write < 0 || pipes_->stdout_read < 0) {
        last_error_ = "Not connected";
        // §MCP-EOF: callers treat null (not {}) as the failure sentinel.
        return json();
    }

    // §fix (2026-07-18): serialize the whole request/response exchange.
    // call_tool_async()/call_tools_batch() run call_tool() on ThreadPool
    // threads against ONE client; without this lock their frames
    // interleave on the pipe and next_request_id_ increments race.
    std::scoped_lock lk(request_mu_);

    // Build JSON-RPC request
    const int request_id = next_request_id_++;
    json request = {
        {"jsonrpc", "2.0"},
        {"id", request_id},
        {"method", method}
    };

    if (!params.is_null()) {
        request["params"] = params;
    }

    // Send request. write_all() loops on short writes and blocks SIGPIPE
    // in the calling thread, so a subprocess that has closed stdin (e.g.
    // it crashed/exited before reading our request) yields EPIPE instead
    // of killing the host.
    std::string request_str = request.dump() + "\n";
    if (!write_all(pipes_->stdin_write, request_str)) {
        last_error_ = "Failed to write to stdin: " + std::string(strerror(errno));
        return json();  // §MCP-EOF: null = failure
    }

    // Read response — only accept the frame whose "id" matches ours.
    return read_response(timeout_ms, request_id);
}

void McpClient::send_notification(const std::string& method, const json& params) {
    if (!pipes_ || pipes_->stdin_write < 0) {
        return;
    }
    
    json notification = {
        {"jsonrpc", "2.0"},
        {"method", method}
    };
    
    if (!params.is_null()) {
        notification["params"] = params;
    }
    
    std::string notif_str = notification.dump() + "\n";
    // write_all() loops on short writes and blocks SIGPIPE: see
    // send_request() comment. Subprocess may have closed stdin between
    // initialize() and this notification.
    [[maybe_unused]] bool sent = write_all(pipes_->stdin_write, notif_str);
}

json McpClient::read_response(int timeout_ms, int expected_id) {
    if (!pipes_ || pipes_->stdout_read < 0) {
        return json();  // §MCP-EOF: null = failure
    }

    std::string buffer;
    struct pollfd pfd = {pipes_->stdout_read, POLLIN, 0};
    int remaining = timeout_ms;

    // §fix (2026-07-18): drain ALL complete '\n'-terminated lines from
    // `buffer` on every pass. Previously only ONE line was processed per
    // poll() wakeup, so a server that coalesced a notification and the
    // real response into a single write left the response sitting in
    // `buffer` while poll() blocked until timeout.
    // Returns the parsed response once its "id" matches expected_id;
    // std::nullopt means "no matching response buffered yet".
    auto drain_lines = [&]() -> std::optional<json> {
        size_t newline_pos;
        while ((newline_pos = buffer.find('\n')) != std::string::npos) {
            std::string line = buffer.substr(0, newline_pos);

            // Skip SSE headers if present
            if (line.size() >= 5 && line.substr(0, 5) == "data:") {
                line = line.substr(5);
                size_t start = line.find_first_not_of(" \t");
                if (start != std::string::npos) {
                    line = line.substr(start);
                }
            }

            // Remove the consumed line from buffer (keep any trailing data)
            buffer = buffer.substr(newline_pos + 1);

            if (line.empty()) {
                continue;
            }

            try {
                json parsed = json::parse(line);

                // §9.7.C — INLINE notification dispatch. A JSON-RPC
                // notification has a "method" field but no "id"
                // (server-initiated, not a response to our request).
                // The only notification we currently care about is
                // `notifications/tools/list_changed`; fire the
                // registered callback and keep reading. This
                // replaces the old separate-thread listener which
                // raced with the main reader for `read()` on the
                // shared stdout fd and could lose response bytes.
                if (!parsed.contains("id") && parsed.contains("method")) {
                    const std::string& m = parsed["method"].get_ref<const std::string&>();
                    if (m == "notifications/tools/list_changed" ||
                        m == "notifications/prompts/list_changed" ||
                        m == "notifications/resources/list_changed") {
                        ListChangedCallback cb_copy;
                        {
                            std::scoped_lock lk(cb_mu_);
                            cb_copy = list_changed_cb_;
                        }
                        if (cb_copy) {
                            try { cb_copy(); }
                            catch (const std::exception& e) {
                                last_error_ = std::string(
                                    "list_changed callback threw: ") + e.what();
                            } catch (...) {
                                last_error_ = "list_changed callback threw (unknown)";
                            }
                        }
                    }
                    // Discard the notification and keep draining the
                    // buffer for our response.
                    continue;
                }

                // §fix (2026-07-18): match the response "id" against the
                // request id. A late response from a timed-out request N
                // must not be consumed as request N+1's result — discard
                // it and keep reading until the matching response arrives
                // (or the overall timeout fires).
                if (expected_id >= 0 &&
                    (!parsed.contains("id") || parsed["id"] != json(expected_id))) {
                    last_error_ = "Discarding response with mismatched id";
                    continue;
                }

                return parsed;
            } catch (const std::exception& e) {
                // Malformed line - log and continue (B4: don't lose data)
                last_error_ = std::string("Malformed JSON-RPC line: ") + e.what();
                // Keep reading - maybe the next line is the real response
            }
        }
        return std::nullopt;
    };

    while (true) {
        // Only attempt to parse COMPLETE lines (terminated by \n), and
        // drain everything already buffered before blocking in poll().
        // B4 FIX: previously, partial lines were dropped on parse failure,
        // which could cause us to lose data if the server writes a large
        // response in multiple chunks.
        if (auto ready = drain_lines()) {
            return *ready;
        }

        int poll_timeout = std::min(remaining, 100);
        int ret = poll(&pfd, 1, poll_timeout);

        if (ret < 0) {
            if (errno == EINTR) {
                continue;
            }
            last_error_ = "Poll error: " + std::string(strerror(errno));
            return json();
        }

        if (ret == 0) {
            // Timeout on this iteration
            if (timeout_ms >= 0) {
                remaining -= 100;
                if (remaining <= 0) {
                    last_error_ = "Timeout waiting for response";
                    return json();
                }
            }
            continue;
        }

        if (pfd.revents & POLLIN) {
            char buf[4096];
            ssize_t n = read(pipes_->stdout_read, buf, sizeof(buf) - 1);

            if (n < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    continue;
                }
                last_error_ = "Read error: " + std::string(strerror(errno));
                return json();
            }

            if (n == 0) {
                // EOF — the server process closed its stdout (typically
                // it crashed or exited during startup). §MCP-EOF: this
                // MUST surface as null so initialize()/connect() fail
                // instead of reporting a phantom success.
                last_error_ = "Server closed connection";
                return json();
            }

            buf[n] = '\0';
            buffer += buf;
            // Newly arrived lines are drained at the top of the loop,
            // before we block in poll() again.
        }

        if (pfd.revents & (POLLHUP | POLLERR | POLLNVAL)) {
            // Drain whatever arrived alongside the hangup (POLLIN and
            // POLLHUP can be set together) before giving up.
            if (auto ready = drain_lines()) {
                return *ready;
            }
            last_error_ = "Server disconnected";
            return json();
        }
    }
}

std::string McpClient::parse_sse_data(const std::string& line) {
    if (line.substr(0, 5) == "data:") {
        return line.substr(5);
    }
    return "";
}

std::string McpClient::read_all_output(int timeout_ms) {
    if (!pipes_ || pipes_->stdout_read < 0) {
        return "";
    }
    
    std::string result;
    struct pollfd pfd = {pipes_->stdout_read, POLLIN, 0};
    int remaining = timeout_ms;
    
    while (true) {
        int poll_timeout = std::min(remaining, 100);
        int ret = poll(&pfd, 1, poll_timeout);
        
        if (ret <= 0) {
            break;
        }
        
        if (pfd.revents & POLLIN) {
            char buf[4096];
            ssize_t n = read(pipes_->stdout_read, buf, sizeof(buf) - 1);
            if (n <= 0) {
                break;
            }
            buf[n] = '\0';
            result += buf;
        } else {
            break;
        }
        
        if (timeout_ms >= 0) {
            remaining -= 100;
            if (remaining <= 0) break;
        }
    }
    
    return result;
}

} // namespace mcp