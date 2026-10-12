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
    // Phase 71: ALWAYS emit (even "[]") — the brain's seed is
    // hub-authoritative for origin='hub' rows, so an empty roster must
    // actively drop previously-seeded hub servers. Agent-local rows
    // (origin='local') are unaffected.
    env_line(body, "HIVEMIND_MCP_SERVERS", spec_.mcp_servers.dump());
    // The synced canonical skill library is mounted read-only at /skills
    // (ensure_container). Project-local /workspace/.agents/skills/ is
    // auto-scanned by hive-brain on its own.
    env_line(body, "HIVEMIND_SKILLS_DIR", "/skills");
    // Phase 60: hub-rendered granted secrets (HIVE_SECRET_<NAME>=…,
    // pre-escaped) — appended verbatim; the file is 0600 worker-local.
    if (!spec_.secret_env.empty())
        body += spec_.secret_env;
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
    if (spec_.host_agent) {
        // Phase 72: bare-metal spawn — the worker's OWN opt-in and the
        // provisioned hivex-<name> user are both mandatory, checked here
        // (defense in depth: the hub's route gate is not trusted).
        if (!hwrunner::g_allow_host_agents)
            throw std::runtime_error(
                "host agent '" + spec_.name + "' refused: this worker has "
                "NOT opted in (--allow-host-agents)");
        if (!hwrunner::host_agent_provisioned(spec_.name))
            throw std::runtime_error(
                "host agent '" + spec_.name + "' refused: run "
                "'sudo hive-worker provision-host-agent " + spec_.name +
                "' on this rig first");
        std::cerr << "HOST AGENT spawn: " << spec_.name << " as "
                  << hwrunner::host_user(spec_.name)
                  << " (sudo, unsandboxed)\n";
    } else {
        // Phase 35: type "gpu" -> gpu image + device passthrough flags
        hwrunner::ensure_container(spec_.id, resolved_image(spec_),
                                   spec_.type == "gpu",
                                   spec_.memory, spec_.cpus);   // Phase 69
    }
    spawn_exec();
    monitor_ = std::thread(&AgentProcess::monitor_loop, this);
}

void AgentProcess::spawn_exec() {
    const std::string env_path = build_env_file();

    int to_child[2], from_child[2];
    if (pipe(to_child) != 0 || pipe(from_child) != 0)
        throw std::runtime_error("pipe failed");

    // Phase 72: host agents bypass podman entirely. The env file (0600,
    // worker-owned — may carry HIVE_SECRET_* lines) is staged into the
    // hivex home as that user, and hive-agent reads it via --env-file so
    // secrets NEVER appear in argv (world-readable via ps). The staged
    // file stays (0600, hivex-owned) — same posture as the worker-local
    // env dir, and respawns re-stage over it.
    const bool host = spec_.host_agent;
    std::vector<std::string> args;         // full argv for the child exec
    const char* prog = "podman";
    if (host) {
        const std::string user = hwrunner::host_user(spec_.name);
        const std::string home = "/home/" + user;
        const std::string staged = home + "/.hive-env";
        // Workspace (cwd / project root): the SHARED
        // --host-workspace-root dir (e.g. the /workspace mount on the
        // host — the same company drive the containers see), else the
        // home dir. NEVER deleted — not by remove(), not by deprovision.
        const std::string ws = hwrunner::host_workspace(spec_.name);
        if (!fs::is_directory(ws))
            throw std::runtime_error(
                "host workspace missing: " + ws +
                " — create it (or fix --host-workspace-root) on this rig");
        std::string out;
        if (hwrunner::run_as_user(user, {"/usr/bin/tee", staged},
                                  env_path, &out) != 0 ||
            hwrunner::run_as_user(user, {"/usr/bin/chmod", "600", staged},
                                  "", &out) != 0)
            throw std::runtime_error("host env staging failed: " + out);
        // No -D/--chdir: sudo-rs (ubuntu-pc) rejects it even with a
        // sudoers CWD= tag. hive-agent chdirs into the project root
        // itself (parity with podman -w /workspace).
        prog = "sudo";
        args = {"-n", "-H", "-u", user,
                "/usr/local/bin/hive-agent", ws, "--env-file", staged};
    } else {
        args = {"exec", "-i", "-u", "hive", "-w", "/workspace",
                "--env-file", env_path,
                hwrunner::container_name(spec_.id), "hive-agent",
                "/workspace"};
    }

    pid_t pid = fork();
    if (pid == 0) {
        dup2(to_child[0], STDIN_FILENO);
        dup2(from_child[1], STDOUT_FILENO);
        // stderr inherited -> worker log
        ::close(to_child[0]); ::close(to_child[1]);
        ::close(from_child[0]); ::close(from_child[1]);
        std::vector<char*> cargs;
        cargs.push_back(const_cast<char*>(prog));
        for (auto& a : args) cargs.push_back(const_cast<char*>(a.c_str()));
        cargs.push_back(nullptr);
        execvp(prog, cargs.data());
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
    for (size_t fi = 0; fi < flush.size(); ++fi) {
        std::string l = flush[fi] + "\n";
        const char* q = l.data(); size_t left = l.size();
        bool ok = true;
        while (left > 0) {
            ssize_t n = ::write(to_child[1], q, left);
            if (n <= 0) { ok = false; break; }
            q += n; left -= (size_t)n;
        }
        if (!ok) {
            // Pipe died mid-flush: requeue this and all remaining frames
            // (front, order preserved) for the NEXT respawn — the dead
            // child took any partial line with it.
            std::lock_guard<std::mutex> lk(m_);
            pending_.insert(pending_.begin(), flush.begin() + (ptrdiff_t)fi,
                            flush.end());
            std::cerr << "fleet: pipe died mid-flush for " << spec_.id
                      << " — requeued " << (flush.size() - fi)
                      << " frame(s)\n";
            break;
        }
        note_frame_delivered(flush[fi]);
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
                // Turn answered: drop it from the in-flight ledger.
                const std::string tid = ev.value("task", "");
                std::lock_guard<std::mutex> lk(m_);
                if (!tid.empty()) {
                    for (auto uit = unanswered_.begin();
                         uit != unanswered_.end(); ++uit)
                        if (uit->first == tid) {
                            unanswered_.erase(uit);
                            break;
                        }
                } else if (!unanswered_.empty()) {
                    unanswered_.pop_front();   // FIFO fallback
                }
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

        // Any task already delivered to the dead exec will never get its
        // "message" event — fail it NOW or the hub-side turn wedges
        // (cancel unconfirmable, queued messages stranded) until the
        // watchdog grace elapses (2026-09-25 PA voice-message wedge).
        fail_unanswered("agent process exited before answering; the task "
                        "did not complete — resend to retry",
                        WIFEXITED(status) ? WEXITSTATUS(status) : -1);

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
            // Phase 72: host agents have no container to ensure — the
            // bare-metal checks run in start() and stay valid across a
            // crash-respawn.
            if (!spec_.host_agent)
                hwrunner::ensure_container(spec_.id, resolved_image(spec_),
                                           spec_.type == "gpu",
                                           spec_.memory, spec_.cpus);  // Phase 69
            spawn_exec();                // creates a fresh reader_ thread
            // A stop() that landed mid-respawn (spec-change restart)
            // couldn't kill a child that didn't exist yet — do it now
            // and loop into waitpid so the child is reaped before exit.
            if (stopping_) { kill_child(); continue; }
            // Crash-respawns reuse the CACHED spec_ — grants/models
            // changed since the last spawn frame would never arrive
            // (the 2026-09-25 vault-grant incident). Ask the hub to push
            // a fresh spawn frame; Fleet::spawn no-ops when nothing
            // changed, so this is free in the steady state.
            if (on_event_)
                on_event_(spec_.id, {{"ev", "spec_refresh_req"}});
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
    const std::string line = op.dump() + "\n";
    std::lock_guard<std::mutex> lk(m_);
    if (stdin_fd_ < 0) {
        pending_.push_back(op.dump());
        std::cerr << "fleet: stdin not ready for " << spec_.id
                  << " — queued (" << pending_.size() << " pending)\n";
        return;
    }
    const char* p = line.data();
    size_t left = line.size();
    while (left > 0) {
        ssize_t n = ::write(stdin_fd_, p, left);
        if (n <= 0) {
            // EPIPE: child gone. Requeue for the respawn flush instead of
            // dropping — a lost task wedges the hub-side turn until the
            // watchdog, and a lost cancel strands a running turn.
            pending_.push_back(op.dump());
            std::cerr << "fleet: stdin write failed for " << spec_.id
                      << " — queued (" << pending_.size() << " pending)\n";
            return;
        }
        p += n; left -= (size_t)n;
    }
    if (op.value("op", "") == "task")
        unanswered_.emplace_back(op.value("id", ""),
                                 op.value("thread_id", ""));
}

void AgentProcess::kill_child() {
    std::lock_guard<std::mutex> lk(m_);
    if (pid_ > 0) ::kill(pid_, SIGKILL);
    if (stdin_fd_ >= 0) { ::close(stdin_fd_); stdin_fd_ = -1; }
}

void AgentProcess::note_frame_delivered(const std::string& frame) {
    json op = json::parse(frame, nullptr, false);
    if (op.is_discarded() || op.value("op", "") != "task") return;
    std::lock_guard<std::mutex> lk(m_);
    unanswered_.emplace_back(op.value("id", ""), op.value("thread_id", ""));
}

void AgentProcess::fail_unanswered(const std::string& why, int exit_status) {
    std::deque<std::pair<std::string, std::string>> dead;
    {
        std::lock_guard<std::mutex> lk(m_);
        dead.swap(unanswered_);
    }
    if (dead.empty() || !on_event_) return;
    std::string err = why;
    if (exit_status >= 0)
        err += " (exit status " + std::to_string(exit_status) + ")";
    for (const auto& [tid, thid] : dead) {
        std::cerr << "fleet: " << spec_.id << " exited with unanswered task "
                  << tid << " — failing it hub-side\n";
        // Synthetic "message" event: main.cpp maps it to task_result,
        // which closes the hub turn (and confirms any pending cancel).
        on_event_(spec_.id, {{"ev", "message"},
                             {"task", tid},
                             {"thread_id", thid},
                             {"body", ""},
                             {"error", err}});
    }
}

void AgentProcess::fail_pending_tasks(const std::string& why) {
    std::vector<std::string> pend;
    {
        std::lock_guard<std::mutex> lk(m_);
        pend.swap(pending_);
    }
    if (!on_event_) return;
    for (const auto& pf : pend) {
        json op = json::parse(pf, nullptr, false);
        if (op.is_discarded() || op.value("op", "") != "task") continue;
        std::cerr << "fleet: " << spec_.id << " stopped with queued task "
                  << op.value("id", "") << " — failing it hub-side\n";
        on_event_(spec_.id, {{"ev", "message"},
                             {"task", op.value("id", "")},
                             {"thread_id", op.value("thread_id", "")},
                             {"body", ""},
                             {"error", why}});
    }
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
    if (reader_.joinable()) reader_.join();   // EOFs once the child dies
    // monitor_loop already failed delivered-but-unanswered tasks; frames
    // still QUEUED (never delivered) would otherwise die silently with
    // this object and wedge their hub-side turns the same way.
    fail_pending_tasks("agent stopped before the task could be delivered "
                       "— resend to retry");
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
    auto it = agents_.find(spec.id);
    if (it != agents_.end() && it->second->spec() == spec)
        return;   // unchanged re-spawn (hub restart reconcile replays
                  // every spawn) — keep the live brain untouched.
    // New agent, or a CHANGED spec for a running one (fresh secret
    // grants, model, prompt…): env loads at exec, so the brain must be
    // restarted for the new spec to take effect. Stop the old process
    // BEFORE starting the new one — two hive-agent execs sharing one
    // container/workspace would corrupt brain state.
    // (Vault-grant incident 2026-09-25: the old "already running" throw
    // discarded every refreshed spawn frame, so grants never propagated
    // across restarts.)
    if (it != agents_.end()) {
        std::cerr << "fleet: spec changed for " << spec.id
                  << " — restarting with fresh spec\n";
        it->second->stop();
        agents_.erase(it);
    }
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
