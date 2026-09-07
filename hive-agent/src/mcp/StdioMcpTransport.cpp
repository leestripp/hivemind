/**
 * @file StdioMcpTransport.cpp
 * @brief stdio MCP transport implementation
 */

#include "StdioMcpTransport.h"

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>
#include <iostream>
#include <sstream>

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

StdioMcpTransport::StdioMcpTransport(std::string command,
                                     std::map<std::string, std::string> env)
    : command_(std::move(command)), env_(std::move(env)) {}

StdioMcpTransport::~StdioMcpTransport() {
    disconnect();
}

std::expected<void, TransportError> StdioMcpTransport::connect() {
    if (connected_.load()) {
        return std::unexpected(TransportError::ConnectionFailed);
    }

    int stdin_pipe[2], stdout_pipe[2];
    if (pipe(stdin_pipe) == -1 || pipe(stdout_pipe) == -1) {
        return std::unexpected(TransportError::ConnectionFailed);
    }

    // Make stdout read end non-blocking
    int flags = fcntl(stdout_pipe[0], F_GETFL, 0);
    fcntl(stdout_pipe[0], F_SETFL, flags | O_NONBLOCK);

    pid_t pid = fork();
    if (pid == -1) {
        close(stdin_pipe[0]); close(stdin_pipe[1]);
        close(stdout_pipe[0]); close(stdout_pipe[1]);
        return std::unexpected(TransportError::ConnectionFailed);
    }

    if (pid == 0) {
        // Child
        close(stdin_pipe[1]);
        close(stdout_pipe[0]);
        dup2(stdin_pipe[0], STDIN_FILENO);
        dup2(stdout_pipe[1], STDOUT_FILENO);
        close(stdin_pipe[0]);
        close(stdout_pipe[1]);

        // §Demo fix: MCP server installers (uv/pip) print progress
        // spinners to stderr. Route stderr to /dev/null so the host
        // terminal stays clean and the CLI prompt isn't overwritten.
        int devnull = ::open("/dev/null", O_WRONLY);
        if (devnull >= 0) {
            ::dup2(devnull, STDERR_FILENO);
            ::close(devnull);
        }

        // Set env
        for (const auto& [k, v] : env_) {
            setenv(k.c_str(), v.c_str(), 1);
        }
        // Ensure PATH
        if (!getenv("PATH")) {
            setenv("PATH", "/home/user/.local/bin:/usr/local/bin:/usr/bin:/bin", 1);
        }

        // Build command
        std::vector<std::string> args;
        std::string current;
        for (char c : command_) {
            if (c == ' ' || c == '\t') {
                if (!current.empty()) { args.push_back(current); current.clear(); }
            } else {
                current += c;
            }
        }
        if (!current.empty()) args.push_back(current);

        std::vector<char*> argv;
        argv.push_back(const_cast<char*>("sh"));
        argv.push_back(const_cast<char*>("-c"));
        std::ostringstream cmd_ss;
        for (size_t i = 0; i < args.size(); ++i) {
            if (i > 0) cmd_ss << " ";
            cmd_ss << args[i];
        }
        std::string full_cmd = cmd_ss.str();
        argv.push_back(const_cast<char*>(full_cmd.c_str()));
        argv.push_back(nullptr);

        execvp("/bin/sh", argv.data());
        _exit(127);
    }

    // Parent
    close(stdin_pipe[0]);
    close(stdout_pipe[1]);

    child_pid_ = pid;
    stdin_write_ = stdin_pipe[1];
    stdout_read_ = stdout_pipe[0];

    // Start reader thread
    connected_.store(true);
    stop_.store(false);
    reader_thread_ = std::thread([this]() { reader_loop(); });

    return {};
}

void StdioMcpTransport::disconnect() {
    if (!connected_.load()) return;
    connected_.store(false);
    stop_.store(true);

    if (reader_thread_.joinable()) {
        reader_thread_.join();
    }

    if (stdin_write_ >= 0) { close(stdin_write_); stdin_write_ = -1; }
    if (stdout_read_ >= 0) { close(stdout_read_); stdout_read_ = -1; }
    if (child_pid_ > 0) {
        kill(child_pid_, SIGTERM);
        int status;
        waitpid(child_pid_, &status, 0);
        child_pid_ = -1;
    }

    // Fail all pending requests
    std::scoped_lock lock(mutex_);
    for (auto& [id, req] : pending_) {
        req->promise.set_value(std::unexpected(TransportError::Cancelled));
    }
    pending_.clear();
}

std::future<std::expected<nlohmann::json, TransportError>>
StdioMcpTransport::send_request(
    const std::string& method,
    const nlohmann::json& params,
    int timeout_ms
) {
    auto promise = std::make_shared<PendingRequest>();
    auto future = promise->promise.get_future();

    if (!connected_.load()) {
        promise->promise.set_value(std::unexpected(TransportError::NotConnected));
        return future;
    }

    int request_id;
    nlohmann::json request;
    {
        std::scoped_lock lock(mutex_);
        request_id = next_request_id_++;
        request = {
            {"jsonrpc", "2.0"},
            {"id", request_id},
            {"method", method}
        };
        if (!params.is_null()) {
            request["params"] = params;
        }
        promise->deadline = std::chrono::steady_clock::now()
                          + std::chrono::milliseconds(timeout_ms);
        pending_[request_id] = promise;
    }

    // Write request. write_all() loops on short writes and blocks
    // SIGPIPE in the calling thread — if the subprocess has closed stdin
    // we get EPIPE back instead of a SIGPIPE that would kill the daemon.
    std::string req_str = request.dump() + "\n";
    if (!write_all(stdin_write_, req_str)) {
        std::scoped_lock lock(mutex_);
        pending_.erase(request_id);
        promise->promise.set_value(std::unexpected(TransportError::WriteFailed));
    }

    return future;
}

std::expected<void, TransportError> StdioMcpTransport::send_notification(
    const std::string& method,
    const nlohmann::json& params
) {
    if (!connected_.load()) {
        return std::unexpected(TransportError::NotConnected);
    }

    nlohmann::json notification = {
        {"jsonrpc", "2.0"},
        {"method", method}
    };
    if (!params.is_null()) {
        notification["params"] = params;
    }

    // write_all() loops on short writes and blocks SIGPIPE: see
    // send_request() above.
    std::string notif_str = notification.dump() + "\n";
    if (!write_all(stdin_write_, notif_str)) {
        return std::unexpected(TransportError::WriteFailed);
    }
    return {};
}

void StdioMcpTransport::reader_loop() {
    std::string buffer;

    // §fix (2026-07-18): helper that fails every pending request — used
    // when the child dies so callers don't hang until their deadlines.
    auto fail_pendings = [this](TransportError err) {
        std::scoped_lock lock(mutex_);
        for (auto& [id, req] : pending_) {
            req->promise.set_value(std::unexpected(err));
        }
        pending_.clear();
    };

    while (!stop_.load() && connected_.load()) {
        struct pollfd pfd = {stdout_read_, POLLIN, 0};
        int ret = poll(&pfd, 1, 200);

        if (ret > 0 && (pfd.revents & POLLIN)) {
            char buf[4096];
            ssize_t n = read(stdout_read_, buf, sizeof(buf) - 1);
            if (n == 0) {
                // EOF - server closed
                stop_.store(true);
                fail_pendings(TransportError::ReadFailed);
                break;
            }
            if (n > 0) {
                buf[n] = '\0';
                buffer += buf;

                // Try to parse complete lines
                size_t pos;
                while ((pos = buffer.find('\n')) != std::string::npos) {
                    std::string line = buffer.substr(0, pos);
                    buffer = buffer.substr(pos + 1);

                    if (line.empty()) continue;

                    try {
                        nlohmann::json response = nlohmann::json::parse(line);
                        if (response.contains("id")) {
                            int id = response["id"].get<int>();
                            std::scoped_lock lock(mutex_);
                            auto it = pending_.find(id);
                            if (it != pending_.end()) {
                                if (response.contains("error")) {
                                    std::string err = response["error"].is_string()
                                        ? response["error"].get<std::string>()
                                        : response["error"].dump();
                                    it->second->promise.set_value(
                                        std::unexpected(TransportError::InvalidResponse));
                                } else if (response.contains("result")) {
                                    it->second->promise.set_value(response["result"]);
                                } else {
                                    it->second->promise.set_value(nlohmann::json::object());
                                }
                                pending_.erase(it);
                            }
                        }
                    } catch (const std::exception& e) {
                        // Malformed line - skip (B4 fix)
                    }
                }
            }
            // n < 0: EAGAIN on the non-blocking fd — fall through to the
            // hangup/deadline checks below.
        }

        // §fix (2026-07-18): handle child exit. poll() reports
        // POLLHUP/POLLERR/POLLNVAL — not POLLIN — when the child dies;
        // the old loop only checked POLLIN and spun at 100% CPU.
        if (ret > 0 && (pfd.revents & (POLLHUP | POLLERR | POLLNVAL))) {
            stop_.store(true);
            fail_pendings(TransportError::ReadFailed);
            break;
        }

        // §fix (2026-07-18): check timeouts EVERY iteration. The old code
        // only checked when poll() returned <= 0, so steady traffic (or
        // a spinning revents mask) starved deadline enforcement.
        {
            auto now = std::chrono::steady_clock::now();
            std::scoped_lock lock(mutex_);
            for (auto it = pending_.begin(); it != pending_.end();) {
                if (it->second->deadline < now) {
                    it->second->promise.set_value(std::unexpected(TransportError::Timeout));
                    it = pending_.erase(it);
                } else {
                    ++it;
                }
            }
        }
    }
}



bool StdioMcpTransport::health_check() {
    if (!connected_.load()) return false;
    if (child_pid_ <= 0) return false;
    // Send signal 0 to check if process exists
    if (kill(child_pid_, 0) != 0) {
        connected_.store(false);
        return false;
    }
    return true;
}

} // namespace mcp
