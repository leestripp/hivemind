// fleet.cpp - v2 fleet manager (Plan.md Phase B). See fleet.h.

#include "fleet.h"
#include "runner.h"

#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace fs = std::filesystem;
using nlohmann::json;

namespace hwfleet {

// -- secrets ---------------------------------------------------------------
// -- AgentProcess ----------------------------------------------------------

AgentProcess::AgentProcess(AgentSpec spec, EventFn on_event)
    : spec_(std::move(spec)), on_event_(std::move(on_event)) {}

AgentProcess::~AgentProcess() { stop(); }

std::string AgentProcess::state() const {
    std::lock_guard<std::mutex> lk(m_);
    return state_;
}

static void set_state(std::mutex& m, std::string& s, const char* v) {
    std::lock_guard<std::mutex> lk(m);
    s = v;
}

// Render KEY=value lines for podman --env-file. NOTE: podman does NOT
// strip quotes - the whole rest of the line is the value verbatim, so no
// quoting here (quoting broke stoul on HIVEMIND_MAX_CONTEXT). Newlines
// are the only hazard; all values we render are single-line by contract
// (the system prompt goes via HIVEMIND_SYSTEM_PROMPT_FILE instead).
static void env_line(std::string& out, const char* key, const std::string& v) {
    if (v.empty()) return;
    out += key;
    out += "=";
    for (char c : v)
        out += (c == '\n') ? ' ' : c;
    out += "\n";
}

std::string AgentProcess::write_prompt_file() {
    if (spec_.system_prompt.empty()) return "";
    const fs::path dir = fs::path(hwrunner::worker_dir())
                       / "workspace" / ".hivemind" / "prompts";
    std::error_code ec;
    fs::create_directories(dir, ec);
    const fs::path f = dir / (spec_.id + ".txt");
    std::ofstream out(f, std::ios::trunc);
    out << spec_.system_prompt;
    return "/workspace/.hivemind/prompts/" + spec_.id + ".txt";
}

std::string AgentProcess::build_env_file() {
    std::string body;
    env_line(body, "HIVEMIND_AGENT_ID", spec_.id);
    env_line(body, "HIVEMIND_AGENT_NAME", spec_.name);
    // Phase 43: no LLM env here — the hub injects provider/model config
    // (endpoint + key + tuning) into every task frame; the brain upserts
    // it live. models.json / secrets.json are gone.
    if (spec_.auto_approve)
        env_line(body, "HIVEMIND_AUTO_APPROVE", "1");
    const std::string prompt_path = write_prompt_file();
    if (!prompt_path.empty())
        env_line(body, "HIVEMIND_SYSTEM_PROMPT_FILE", prompt_path);
    if (!spec_.mcp_servers.empty())
        env_line(body, "HIVEMIND_MCP_SERVERS", spec_.mcp_servers.dump());
    // The synced canonical skill library is mounted read-only at /skills
    // (ensure_container). Project-local /workspace/.agents/skills/ is
    // auto-scanned by hive-brain on its own.
    env_line(body, "HIVEMIND_SKILLS_DIR", "/skills");
    // Phase 50: per-skill gate (comma-joined; hive-agent parses at startup)
    if (!spec_.disabled_skills.empty()) {
        std::string joined;
        for (const auto& n : spec_.disabled_skills) {
            if (!joined.empty()) joined += ',';
            joined += n;
        }
        env_line(body, "HIVEMIND_SKILLS_DISABLED", joined);
    }

    const fs::path dir = fs::path(hwrunner::worker_dir()) / "env";
    std::error_code ec;
    fs::create_directories(dir, ec);
    const fs::path f = dir / (spec_.id + ".env");
    {
        std::ofstream out(f, std::ios::trunc);
        out << body;
    }
    ::chmod(f.c_str(), 0600);
    return f.string();
}

void AgentProcess::start() {
    {
        std::lock_guard<std::mutex> lk(m_);
        if (state_ != "down") throw std::runtime_error("already started");
        state_ = "starting";
    }
    stopping_ = false;
    // Phase 43: no model resolution here — the hub injects the full
    // provider/model config into every task frame (llm).
    // Phase 35: type "gpu" -> gpu image + device passthrough flags
    hwrunner::ensure_container(spec_.id, resolved_image(spec_),
                               spec_.type == "gpu");
    spawn_exec();
    monitor_ = std::thread(&AgentProcess::monitor_loop, this);
}

void AgentProcess::spawn_exec() {
    const std::string env_path = build_env_file();
    const std::string cname = hwrunner::container_name(spec_.id);

    int to_child[2], from_child[2];
    if (pipe(to_child) != 0 || pipe(from_child) != 0)
        throw std::runtime_error("pipe failed");

    std::vector<std::string> args{
        "exec", "-i", "-u", "hive", "-w", "/workspace",
        "--env-file", env_path, cname, "hive-agent", "/workspace"};

    pid_t pid = fork();
    if (pid == 0) {
        dup2(to_child[0], STDIN_FILENO);
        dup2(from_child[1], STDOUT_FILENO);
        // stderr inherited -> worker log
        ::close(to_child[0]); ::close(to_child[1]);
        ::close(from_child[0]); ::close(from_child[1]);
        std::vector<char*> cargs;
        cargs.push_back(const_cast<char*>("podman"));
        for (auto& a : args) cargs.push_back(const_cast<char*>(a.c_str()));
        cargs.push_back(nullptr);
        execvp("podman", cargs.data());
        _exit(127);
    }
    ::close(to_child[0]);
    ::close(from_child[1]);
    if (pid < 0) {
        ::close(to_child[1]); ::close(from_child[0]);
        throw std::runtime_error("fork failed");
    }
    std::vector<std::string> flush;
    {
        std::lock_guard<std::mutex> lk(m_);
        pid_ = pid;
        stdin_fd_ = to_child[1];
        flush.swap(pending_);
    }
    for (const auto& pf : flush) {
        std::string l = pf + "\n";
        const char* q = l.data(); size_t left = l.size();
        while (left > 0) {
            ssize_t n = ::write(to_child[1], q, left);
            if (n <= 0) break;
            q += n; left -= (size_t)n;
        }
    }
    if (!flush.empty())
        std::cerr << "fleet: flushed " << flush.size()
                  << " queued frame(s) to " << spec_.id << "\n";
    // Caller guarantees any previous reader_ is joined (start: fresh;
    // monitor_loop: joins before respawn).
    reader_ = std::thread(&AgentProcess::reader_loop, this, from_child[0]);
    std::cerr << "fleet: spawned hive-agent for " << spec_.id
              << " (exec pid " << pid << ")\n";
}

void AgentProcess::reader_loop(int fd) {
    std::string buf;
    char chunk[8192];
    for (;;) {
        ssize_t n = ::read(fd, chunk, sizeof chunk);
        if (n <= 0) break;                 // EOF: child stdout closed
        buf.append(chunk, (size_t)n);
        size_t nl;
        while ((nl = buf.find('\n')) != std::string::npos) {
            std::string line = buf.substr(0, nl);
            buf.erase(0, nl + 1);
            if (line.empty()) continue;
            json ev;
            try { ev = json::parse(line); }
            catch (...) { continue; }
            const std::string kind = ev.value("ev", "");
            if (kind == "ready") {
                set_state(m_, state_, "ready");
                restart_backoff_s_ = 1;    // healthy spawn resets backoff
            } else if (kind == "delta") {
                set_state(m_, state_, "acting");
            } else if (kind == "message") {
                set_state(m_, state_, "ready");
            } else if (kind == "status") {
                // Brain state machine (idle/understanding/acting/...) from
                // hive-agent's on_state_change; lifecycle states
                // (starting/restarting/down) remain monitor-owned.
                const std::string st = ev.value("state", "");
                if (!st.empty()) set_state(m_, state_, st.c_str());
            }
            if (on_event_) on_event_(spec_.id, ev);
        }
    }
    ::close(fd);
}

void AgentProcess::monitor_loop() {
    for (;;) {
        pid_t pid;
        {
            std::lock_guard<std::mutex> lk(m_);
            pid = pid_;
        }
        int status = 0;
        waitpid(pid, &status, 0);
        {
            std::lock_guard<std::mutex> lk(m_);
            if (stdin_fd_ >= 0) { ::close(stdin_fd_); stdin_fd_ = -1; }
            pid_ = -1;
        }
        if (reader_.joinable()) reader_.join();

        if (stopping_) {
            set_state(m_, state_, "down");
            return;
        }
        // Unexpected exit: respawn with backoff, KEEP MONITORING (this
        // thread is the only monitor ever - thread objects are never
        // reassigned while running). Brain state persists on /workspace,
        // so the agent resumes with history.
        set_state(m_, state_, "restarting");
        if (on_event_)
            on_event_(spec_.id, {{"ev", "status"},
                                 {"state", "restarting"},
                                 {"exit", WIFEXITED(status)
                                              ? WEXITSTATUS(status) : -1}});
        const int wait_s = restart_backoff_s_;
        restart_backoff_s_ = std::min(restart_backoff_s_ * 2, 60);
        for (int i = 0; i < wait_s * 10 && !stopping_; ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        if (stopping_) { set_state(m_, state_, "down"); return; }
        try {
            hwrunner::ensure_container(spec_.id, resolved_image(spec_),
                                       spec_.type == "gpu");
            spawn_exec();                // creates a fresh reader_ thread
        } catch (const std::exception& e) {
            std::cerr << "fleet: respawn failed for " << spec_.id << ": "
                      << e.what() << "\n";
            set_state(m_, state_, "down");
            if (on_event_)
                on_event_(spec_.id, {{"ev", "status"},
                                     {"state", "down"},
                                     {"error", e.what()}});
            return;
        }
    }
}

void AgentProcess::send(const json& op) {
    std::lock_guard<std::mutex> lk(m_);
    if (stdin_fd_ < 0) {
        pending_.push_back(op.dump());
        std::cerr << "fleet: stdin not ready for " << spec_.id
                  << " — queued (" << pending_.size() << " pending)\n";
        return;
    }
    const std::string line = op.dump() + "\n";
    const char* p = line.data();
    size_t left = line.size();
    while (left > 0) {
        ssize_t n = ::write(stdin_fd_, p, left);
        if (n <= 0) break;                 // EPIPE: child gone
        p += n; left -= (size_t)n;
    }
}

void AgentProcess::kill_child() {
    std::lock_guard<std::mutex> lk(m_);
    if (pid_ > 0) ::kill(pid_, SIGKILL);
    if (stdin_fd_ >= 0) { ::close(stdin_fd_); stdin_fd_ = -1; }
}

void AgentProcess::stop() {
    if (stopping_.exchange(true)) {
        // already stopping: just make sure threads are joined
        if (monitor_.joinable()) monitor_.join();
        return;
    }
    send({{"op", "shutdown"}});
    // grace: hive-agent persists history on shutdown
    for (int i = 0; i < 150; ++i) {
        {
            std::lock_guard<std::mutex> lk(m_);
            if (pid_ < 0) break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    kill_child();
    if (monitor_.joinable()) monitor_.join();
    set_state(m_, state_, "down");
    const fs::path envf = fs::path(hwrunner::worker_dir()) / "env"
                        / (spec_.id + ".env");
    std::error_code ec;
    fs::remove(envf, ec);
    std::cerr << "fleet: stopped " << spec_.id << "\n";
}

// -- Fleet -----------------------------------------------------------------

void Fleet::spawn(const AgentSpec& spec) {
    std::lock_guard<std::mutex> lk(m_);
    if (agents_.count(spec.id))
        throw std::runtime_error("agent already running: " + spec.id);
    auto proc = std::make_unique<AgentProcess>(spec, on_event_);
    proc->start();                       // throws on infra failure
    agents_.emplace(spec.id, std::move(proc));
}

void Fleet::stop(const std::string& agent_id) {
    std::unique_ptr<AgentProcess> proc;
    {
        std::lock_guard<std::mutex> lk(m_);
        auto it = agents_.find(agent_id);
        if (it == agents_.end()) return;
        proc = std::move(it->second);
        agents_.erase(it);
    }
    proc->stop();                        // outside the map lock
}

void Fleet::remove(const std::string& agent_id) {
    stop(agent_id);                       // brain down, if it was up
    // -t 0: the container's PID 1 (sleep infinity) ignores SIGTERM -
    // without this rm -f stalls the full 10s stop timeout.
    std::string out;
    hwrunner::podman_simple(
        {"rm", "-f", "-t", "0", hwrunner::container_name(agent_id)}, &out);
    std::error_code ec;
    fs::remove(fs::path(hwrunner::worker_dir()) / "env"
               / (agent_id + ".env"), ec);
    fs::remove_all(fs::path(hwrunner::worker_dir())
                   / ("home-" + agent_id), ec);
}

bool Fleet::send_to(const std::string& agent_id, const json& op) {
    std::lock_guard<std::mutex> lk(m_);
    auto it = agents_.find(agent_id);
    if (it == agents_.end()) return false;
    it->second->send(op);
    return true;
}


json Fleet::statuses() const {
    json out = json::object();
    std::lock_guard<std::mutex> lk(m_);
    for (const auto& kv : agents_) out[kv.first] = kv.second->state();
    return out;
}

} // namespace hwfleet
