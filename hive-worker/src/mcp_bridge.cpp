// src/mcp_bridge.cpp — Phase 33 (worker side). See mcp_bridge.h.

#include "mcp_bridge.h"

#include <poll.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <cstring>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>

namespace {

struct Proc {
    pid_t pid = -1;
    int fd_in = -1;    // we write → child stdin
    int fd_out = -1;   // child stdout → we read
    std::mutex m;      // serializes rpcs (MCP stdio is sequential)
    std::chrono::steady_clock::time_point last = std::chrono::steady_clock::now();
    std::string partial;   // read leftover across poll cycles
};
using ProcPtr = std::shared_ptr<Proc>;

std::mutex g_map_m;
std::map<std::string, ProcPtr> g_procs;   // key: agent + '\0' + server

std::string container_name(const std::string& agent) {
    std::string out = "hive-";
    for (char c : agent)
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' ||
            (c >= 'A' && c <= 'Z'))
            out += c;
    return out;
}

void kill_proc(Proc& p) {
    if (p.pid > 0) {
        ::kill(p.pid, SIGKILL);
        int st = 0;
        while (waitpid(p.pid, &st, 0) < 0 && errno == EINTR) {}
    }
    if (p.fd_in >= 0) close(p.fd_in);
    if (p.fd_out >= 0) close(p.fd_out);
    p.pid = -1;
    p.fd_in = p.fd_out = -1;
    p.partial.clear();
}

bool spawn(Proc& p, const std::string& agent, const std::string& command,
           const std::vector<std::string>& args, const nlohmann::json& env,
           std::string& err) {
    int in_pipe[2], out_pipe[2];
    if (pipe(in_pipe) != 0 || pipe(out_pipe) != 0) {
        err = "pipe failed";
        return false;
    }
    pid_t pid = fork();
    if (pid < 0) {
        err = "fork failed";
        close(in_pipe[0]); close(in_pipe[1]);
        close(out_pipe[0]); close(out_pipe[1]);
        return false;
    }
    if (pid == 0) {
        // child: stdin ← in_pipe[0], stdout → out_pipe[1]; stderr inherited
        // (lands in the worker journal — MCP servers log there).
        dup2(in_pipe[0], STDIN_FILENO);
        dup2(out_pipe[1], STDOUT_FILENO);
        close(in_pipe[0]); close(in_pipe[1]);
        close(out_pipe[0]); close(out_pipe[1]);
        std::vector<std::string> a{"podman", "exec", "-i"};  // argv[0]!
        if (env.is_object()) {
            for (auto& [k, v] : env.items()) {
                if (!v.is_string()) continue;
                a.push_back("-e");
                a.push_back(k + "=" + v.get<std::string>());
            }
        }
        a.push_back(container_name(agent));
        a.push_back(command);
        for (const auto& x : args) a.push_back(x);
        std::vector<char*> argv;
        for (auto& s : a) argv.push_back(s.data());
        argv.push_back(nullptr);
        execvp("podman", argv.data());
        _exit(127);
    }
    close(in_pipe[0]);
    close(out_pipe[1]);
    p.pid = pid;
    p.fd_in = in_pipe[1];
    p.fd_out = out_pipe[0];
    p.partial.clear();
    std::cerr << "[mcp] spawned " << command << " in "
              << container_name(agent) << " (pid " << pid << ")\n";
    return true;
}

// Write all bytes; false on failure.
bool write_all(int fd, const std::string& data) {
    size_t off = 0;
    while (off < data.size()) {
        ssize_t n = write(fd, data.data() + off, data.size() - off);
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        off += (size_t)n;
    }
    return true;
}

} // namespace

std::string hwbridge::rpc(const std::string& agent, const std::string& server,
                          const std::string& command,
                          const std::vector<std::string>& args,
                          const nlohmann::json& env,
                          const std::string& payload,
                          int timeout_s, std::string& err) {
    const std::string key = agent + '\0' + server;
    ProcPtr p;
    {
        std::lock_guard lk(g_map_m);
        auto& slot = g_procs[key];
        if (!slot) slot = std::make_shared<Proc>();
        p = slot;
    }

    std::lock_guard plk(p->m);   // serialize per (agent, server)

    // What response id do we wait for? (notifications: none)
    nlohmann::json want_id;
    bool notification = true;
    try {
        auto rpc = nlohmann::json::parse(payload);
        if (rpc.contains("id") && !rpc["id"].is_null()) {
            want_id = rpc["id"];
            notification = false;
        }
    } catch (...) {
        err = "invalid JSON-RPC payload";
        return "";
    }

    // (Re)spawn if the process died.
    if (p->pid <= 0 || ::kill(p->pid, 0) != 0) {
        kill_proc(*p);
        if (!spawn(*p, agent, command, args, env, err)) return "";
    }
    p->last = std::chrono::steady_clock::now();

    if (!write_all(p->fd_in, payload + "\n")) {
        // EPIPE etc. — respawn once and retry the write.
        kill_proc(*p);
        if (!spawn(*p, agent, command, args, env, err)) return "";
        p->last = std::chrono::steady_clock::now();
        if (!write_all(p->fd_in, payload + "\n")) {
            err = "write to MCP server failed";
            return "";
        }
    }

    if (notification) return R"({"ok":true})";   // nothing to wait for

    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(timeout_s);
    char buf[64 * 1024];
    for (;;) {
        // Complete lines already buffered?
        for (;;) {
            const auto nl = p->partial.find('\n');
            if (nl == std::string::npos) break;
            std::string line = p->partial.substr(0, nl);
            p->partial.erase(0, nl + 1);
            if (line.empty()) continue;
            try {
                auto msg = nlohmann::json::parse(line);
                if (msg.contains("id") && msg["id"] == want_id)
                    return line;
                // Server→client notification/request — log + keep waiting.
                std::cerr << "[mcp] ignoring server msg: "
                          << line.substr(0, 120) << "\n";
            } catch (...) { /* partial/garbage line — skip */ }
        }

        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) break;
        const int ms = (int)std::chrono::duration_cast<std::chrono::milliseconds>(
                           deadline - now).count();
        pollfd pfd{p->fd_out, POLLIN, 0};
        const int pr = poll(&pfd, 1, ms);
        if (pr < 0) {
            if (errno == EINTR) continue;
            err = "poll failed";
            break;
        }
        if (pr == 0) break;   // timeout
        if (pfd.revents & (POLLERR | POLLHUP)) {
            err = "MCP server closed stdout";
            break;
        }
        const ssize_t n = read(p->fd_out, buf, sizeof(buf));
        if (n <= 0) { err = "MCP server closed stdout"; break; }
        p->partial.append(buf, (size_t)n);
        p->last = std::chrono::steady_clock::now();
    }

    // Process state is unknown after a failed exchange — kill it so the
    // next rpc respawns a clean server.
    kill_proc(*p);
    if (err.empty()) err = "mcp rpc timed out";
    return "";
}

void hwbridge::reap_idle() {
    const auto cutoff =
        std::chrono::steady_clock::now() - std::chrono::minutes(10);
    std::vector<ProcPtr> victims;
    {
        std::lock_guard lk(g_map_m);
        for (auto it = g_procs.begin(); it != g_procs.end();) {
            if (it->second->last < cutoff && it->second->m.try_lock()) {
                victims.push_back(it->second);
                it->second->m.unlock();
                it = g_procs.erase(it);
            } else {
                ++it;
            }
        }
    }
    for (auto& p : victims) kill_proc(*p);
}

void hwbridge::shutdown_all() {
    std::vector<ProcPtr> all;
    {
        std::lock_guard lk(g_map_m);
        for (auto& [k, p] : g_procs) all.push_back(p);
        g_procs.clear();
    }
    for (auto& p : all) {
        std::lock_guard plk(p->m);
        kill_proc(*p);
    }
}
