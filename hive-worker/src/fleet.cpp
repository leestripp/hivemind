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

std::string resolve_api_key(const std::string& provider) {
    const fs::path p = fs::path(hwrunner::worker_dir()) / "secrets.json";
    std::ifstream in(p);
    if (!in)
        throw std::runtime_error("worker secrets missing: " + p.string() +
                                 " (create 0600, {\"<provider>\": \"<key>\"})");
    json j;
    try { in >> j; }
    catch (...) { throw std::runtime_error("worker secrets: bad json"); }
    const std::string key = j.value(provider, "");
    if (key.empty())
        throw std::runtime_error("worker secrets: no key for provider '" +
                                 provider + "'");
    return key;
}

/// Resolve a model NAME to its full config from the worker-local
/// models.json (~/.local/share/hive-worker/models.json, 0600):
///   {"<name>": {"model_id","host","endpoint","provider",
///               "max_context","temperature"}}
/// Fields missing from the entry keep the spec's own values.
/// models.json -> [{name, provider}] for the hello catalog.
json list_worker_models() {
    const fs::path p = fs::path(hwrunner::worker_dir()) / "models.json";
    std::ifstream in(p);
    json out = json::array();
    if (!in) return out;
    json j;
    try { in >> j; } catch (...) { return out; }
    for (auto& [name, e] : j.items())
        if (e.is_object())   // skip the "default" string convention key
            out.push_back({{"name", name},
                           {"provider", e.value("provider", "")}});
    return out;
}

std::string worker_default_model() {
    const fs::path p = fs::path(hwrunner::worker_dir()) / "models.json";
    std::ifstream in(p);
    if (!in) return "";
    json j;
    try { in >> j; } catch (...) { return ""; }
    if (j.contains("default") && j["default"].is_string() &&
        j.contains(j["default"].get<std::string>()))
        return j["default"].get<std::string>();
    for (auto& [name, e] : j.items())
        if (e.is_object()) return name;
    return "";
}

std::string set_default_model(const std::string& name) {
    const fs::path p = fs::path(hwrunner::worker_dir()) / "models.json";
    std::ifstream in(p);
    if (!in) return "models.json missing on worker";
    json j;
    try { in >> j; } catch (...) { return "models.json: bad json"; }
    auto it = j.find(name);
    if (it == j.end() || !it->is_object()) return "unknown model: " + name;
    j["default"] = name;
    std::ofstream out(p, std::ios::trunc);
    if (!out) return "models.json: write failed";
    out << j.dump(2) << "\n";
    return "";
}

void resolve_model(AgentSpec& spec) {
    const fs::path p = fs::path(hwrunner::worker_dir()) / "models.json";
    std::ifstream in(p);
    if (!in)
        throw std::runtime_error("worker models missing: " + p.string() +
            " (create 0600, {\"<name>\": {\"model_id\",\"host\",...}})");
    json j;
    try { in >> j; }
    catch (...) { throw std::runtime_error("worker models: bad json"); }
    // "" = worker default. Convention: a top-level "default" key names
    // the entry to use; without one, the alphabetically-first entry wins
    // (nlohmann objects are ordered maps). Previously "" always threw,
    // so UI "worker default" selections could never spawn.
    if (spec.model.empty()) {
        if (j.contains("default") && j["default"].is_string() &&
            j.contains(j["default"].get<std::string>()))
            spec.model = j["default"].get<std::string>();
        else
            for (auto& [name, e2] : j.items())
                if (e2.is_object()) { spec.model = name; break; }
        if (spec.model.empty())
            throw std::runtime_error("worker models: no usable entries");
    }
    if (!j.contains(spec.model))
        throw std::runtime_error("worker models: no entry for model '" +
                                 spec.model + "'");
    const json& e = j[spec.model];
    const std::string mid = e.value("model_id", "");
    if (!mid.empty()) spec.model = mid;        // name -> remote model id
    if (spec.api_host.empty())
        spec.api_host = e.value("host", "");
    if (spec.chat_endpoint.empty())
        spec.chat_endpoint = e.value("endpoint", "/v1/chat/completions");
    if (spec.provider.empty())
        spec.provider = e.value("provider", "");
    if (spec.max_context <= 0)
        spec.max_context = e.value("max_context", 0L);
    if (spec.temperature < 0)
        spec.temperature = e.value("temperature", -1.0);
    if (spec.api_host.empty() || spec.provider.empty())
        throw std::runtime_error("worker models: entry for '" + spec.model +
                                 "' lacks host/provider");
}

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
    env_line(body, "HIVEMIND_MODEL", spec_.model);
    env_line(body, "HIVEMIND_API_HOST", spec_.api_host);
    env_line(body, "HIVEMIND_API_KEY", resolve_api_key(spec_.provider));
    env_line(body, "HIVEMIND_CHAT_ENDPOINT", spec_.chat_endpoint);
    if (spec_.max_context > 0)
        env_line(body, "HIVEMIND_MAX_CONTEXT",
                 std::to_string(spec_.max_context));
    if (spec_.temperature >= 0)
        env_line(body, "HIVEMIND_TEMPERATURE",
                 std::to_string(spec_.temperature));
    if (spec_.auto_approve)
        env_line(body, "HIVEMIND_AUTO_APPROVE", "1");
    const std::string prompt_path = write_prompt_file();
    if (!prompt_path.empty())
        env_line(body, "HIVEMIND_SYSTEM_PROMPT_FILE", prompt_path);
    if (!spec_.mcp_servers.empty())
        env_line(body, "HIVEMIND_MCP_SERVERS", spec_.mcp_servers.dump());
    // The synced canonical skill library is mounted read-only at /skills
    // (ensure_container). Project-local /workspace/.agents/skills/ is
    // auto-scanned by daedalus on its own.
    env_line(body, "HIVEMIND_SKILLS_DIR", "/skills");

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
    resolve_model(spec_);            // model/provider names -> full config
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
    {
        std::lock_guard<std::mutex> lk(m_);
        pid_ = pid;
        stdin_fd_ = to_child[1];
    }
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
    if (stdin_fd_ < 0) return;
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
