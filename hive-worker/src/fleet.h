#pragma once

// fleet.h - v2 fleet manager (Plan.md Phase B).
//
// The worker owns agent LIFECYCLE: one container per agent (the
// environment: volumes, user, network) plus one hive-agent process per
// agent (the brain: `podman exec -i ... hive-agent /workspace`) whose
// stdin/stdout pipes the worker holds directly. NDJSON ops go in
// (task/cancel/shutdown), NDJSON events come out
// (ready/delta/message/error) and are forwarded to the
// server over the worker channel.
//
// Credentials never transit the server: agent_spawn carries everything
// EXCEPT api keys; the worker resolves keys from its own local
// secrets file (~/.local/share/hive-worker/secrets.json, 0600,
// {"<provider>": "<key>"}) keyed by the spec's provider name.

#include <nlohmann/json.hpp>

#include <atomic>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>

namespace hwfleet {

// Phase 35 chain: :latest base → :browser → :gpu. The default is :latest
// (the legacy :v2 tag stranded non-gpu agents on pre-chain images forever —
// hit 2026-09-03: Chief ran a daedalus-0.9.6 :v2 while :latest was 1.0.3).
inline constexpr const char* kImageLatest = "localhost/hive-agent:latest";
// Phase 35: GPU workhorse image (Containerfile.hive-agent-gpu)
inline constexpr const char* kImageGpu = "localhost/hive-agent:gpu";

struct AgentSpec {
    std::string id;            // server roster id (container hive-<id>)
    std::string name;          // display/brain name (HIVEMIND_AGENT_NAME)
    std::string model;         // remote model id
    std::string api_host;
    std::string chat_endpoint;
    std::string provider;      // key into secrets.json
    long    max_context = 0;
    double  temperature = -1;
    std::string system_prompt; // written to the shared volume, passed by path
    nlohmann::json mcp_servers = nlohmann::json::array();
    std::string image;         // "" -> kImageLatest (verbatim override wins)
    std::string type;          // Phase 35: "" -> standard, "gpu" -> kImageGpu + devices
    std::string memory;        // Phase 69: podman size ("4g"); "" -> worker default
    double cpus = 0;           // Phase 69: CPU cap; 0 -> worker default
    std::vector<std::string> disabled_skills;  // Phase 50: hub's per-skill gate
    std::string secret_env;      // Phase 60: hub-rendered HIVE_SECRET_* lines
    // Phase 72: run bare-metal as unix user hivex-<name> (sudo, NO
    // sandbox) instead of a podman container. The worker's own
    // --allow-host-agents opt-in is verified at spawn — this flag alone
    // is never sufficient.
    bool    host_agent = false;

    /// Whole-spec compare: a re-sent agent_spawn with an IDENTICAL spec is
    /// a no-op (hub restart reconcile replays every spawn); a changed one
    /// (new secret grants, model, prompt…) restarts the brain so the new
    /// config actually takes effect (Phase 65).
    bool operator==(const AgentSpec&) const = default;
};

// Phase 35: explicit image wins; otherwise type selects the image.
// Inline here (not static in fleet.cpp) so tests/v2/test_fleet can
// verify the type -> image mapping without spawning anything.
inline std::string resolved_image(const AgentSpec& s) {
    if (!s.image.empty()) return s.image;
    return s.type == "gpu" ? kImageGpu : kImageLatest;
}

// ── Runtime descriptor (2026-10-12, Plan.md "one binary, environment-
// aware tools") ─────────────────────────────────────────────────────
// The SAME hive-agent binary runs in containers and bare-metal; the
// worker tells it which world it landed in via env. Pure + inline so
// tests/v2/test_fleet can verify the rendering without a worker.
struct RuntimeEnv {
    std::string runtime;          // "container" | "host"
    std::string workspace_root;   // "/workspace" or the host path
    std::string skills_dir;       // "/skills" or the host library path
    std::string service_host;     // "" -> service sharing disabled
    int service_port_base = 0;    // host: identity mapping (agent binds
    int service_port_count = 0;   // the advertised port directly)
};

// Newline-flattening matches fleet.cpp's env_line (env files are
// line-based; all rendered values are single-line by contract).
inline std::string runtime_env_lines(const RuntimeEnv& e) {
    std::string out;
    auto line = [&out](const char* key, const std::string& v) {
        if (v.empty()) return;
        out += key;
        out += "=";
        for (char c : v) out += (c == '\n') ? ' ' : c;
        out += "\n";
    };
    line("HIVEMIND_RUNTIME", e.runtime);
    line("HIVEMIND_WORKSPACE_ROOT", e.workspace_root);
    line("HIVEMIND_SKILLS_DIR", e.skills_dir);
    if (e.service_port_base > 0 && e.service_port_count > 0) {
        line("HIVEMIND_SERVICE_HOST", e.service_host);
        line("HIVEMIND_SERVICE_PORT_BASE", std::to_string(e.service_port_base));
        line("HIVEMIND_SERVICE_PORT_COUNT", std::to_string(e.service_port_count));
        // Identity mapping: no NAT outside a container, the agent binds
        // the advertised port itself (container_base == port_base).
        line("HIVEMIND_SERVICE_CONTAINER_BASE", std::to_string(e.service_port_base));
    }
    return out;
}

/// One running (or restarting) hive-agent session.
class AgentProcess {
public:
    using EventFn = std::function<void(const std::string& agent_id,
                                       const nlohmann::json& ev)>;

    AgentProcess(AgentSpec spec, EventFn on_event);
    ~AgentProcess();

    AgentProcess(const AgentProcess&) = delete;
    AgentProcess& operator=(const AgentProcess&) = delete;

    /// Throws std::runtime_error on infra failure (podman, missing key).
    void start();
    void stop();            // graceful: shutdown op, then kill
    void send(const nlohmann::json& op);   // NDJSON line -> child stdin

    const std::string& id() const { return spec_.id; }
    /// The spec this process was spawned with (for respawn dedup).
    const AgentSpec& spec() const { return spec_; }
    /// down | starting | ready | acting | restarting
    std::string state() const;

private:
    void spawn_exec();      // fork + podman exec -i, reader+monitor threads
    void reader_loop(int fd);
    void monitor_loop();
    void kill_child();
    /// Record a flushed pending_ frame as delivered (task frames only).
    void note_frame_delivered(const std::string& frame);
    /// Emit synthetic error "message" events for every delivered-but-
    /// unanswered task; clears the ledger. exit_status < 0 = signal.
    void fail_unanswered(const std::string& why, int exit_status);
    /// Fail any task frames still sitting in pending_ (never delivered)
    /// and clear the queue — called by stop(), where the frames would
    /// otherwise die silently with this object.
    void fail_pending_tasks(const std::string& why);
    std::string build_env_file();   // renders HIVEMIND_* env, returns path
    std::string write_prompt_file(); // -> /workspace/.hivemind/prompts/<id>.txt

    AgentSpec spec_;
    EventFn on_event_;

    mutable std::mutex m_;
    pid_t pid_ = -1;
    int stdin_fd_ = -1;
    // Frames that arrived while the exec pipe was down (container boot)
    // — flushed by spawn_exec once stdin_fd_ is live. Without this the
    // first task after a cold spawn raced the exec and vanished silently
    // (hit 2026-09-07: Forge's first Phase-43 task dropped mid-boot).
    std::vector<std::string> pending_;
    // Tasks delivered to the child (direct write or pending_ flush) but
    // not yet answered by a "message" event. If the exec dies with
    // entries here the hub-side turn would wedge (cancel unconfirmable,
    // queued messages stranded) until the watchdog grace — so
    // monitor_loop/stop() synthesise error "message" events for them
    // (2026-09-25 PA wedge: exec died mid-turn, thread stuck ~6 min).
    std::deque<std::pair<std::string, std::string>> unanswered_;  // (task, thread_id)
    std::string state_ = "down";
    std::atomic<bool> stopping_{false};
    int restart_backoff_s_ = 1;
    std::thread reader_;
    std::thread monitor_;
};

/// The worker's roster: agent id -> process.
class Fleet {
public:
    using EventFn = AgentProcess::EventFn;

    explicit Fleet(EventFn on_event) : on_event_(std::move(on_event)) {}

    /// agent_spawn op from the server. Throws on error.
    /// Re-spawn of a RUNNING agent: identical spec = no-op (the hub
    /// replays every spawn on reconnect); changed spec = graceful restart
    /// with the new spec so env-affecting updates (Phase 60 secret grants,
    /// model, prompt) propagate without a worker restart.
    void spawn(const AgentSpec& spec);
    void stop(const std::string& agent_id);
    /// Roster delete: stop the brain AND destroy its container + env file
    /// + home dir (best-effort; agent row is already gone server-side).
    void remove(const std::string& agent_id);
    /// Route an op (task / cancel) to an agent.
    /// Returns false if the agent is not running here.
    bool send_to(const std::string& agent_id, const nlohmann::json& op);

    /// {agent_id: state} for heartbeats.
    nlohmann::json statuses() const;

    /// File-op workspace root for an agent: the HOST agent's real
    /// workspace, "" for container agents (caller falls back to the
    /// shared staging workspace) and unknown ids.
    std::string workspace_root(const std::string& agent_id) const;
    /// The hivex-<name> unix user of a HOST agent running here;
    /// "" for container agents and unknown ids.
    std::string host_user_of(const std::string& agent_id) const;

private:
    EventFn on_event_;
    mutable std::mutex m_;
    std::unordered_map<std::string, std::unique_ptr<AgentProcess>> agents_;
};






/// Resolve a model NAME to its full config from the worker-local
/// ── Model-registry management (unification Phase 2) ──────────────
/// The hub RELAYS these over the worker channel; models.json and
/// secrets.json stay worker-local (the hub never holds credentials).
/// All return an error string ("" = success) except list_models_detail.






} // namespace hwfleet
