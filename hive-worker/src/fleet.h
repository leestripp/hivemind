#pragma once

// fleet.h - v2 fleet manager (Plan.md Phase B).
//
// The worker owns agent LIFECYCLE: one container per agent (the
// environment: volumes, user, network) plus one hive-agent process per
// agent (the brain: `podman exec -i ... hive-agent /workspace`) whose
// stdin/stdout pipes the worker holds directly. NDJSON ops go in
// (task/approval_resp/cancel/shutdown), NDJSON events come out
// (ready/delta/message/approval_request/error) and are forwarded to the
// server over the worker channel.
//
// Credentials never transit the server: agent_spawn carries everything
// EXCEPT api keys; the worker resolves keys from its own local
// secrets file (~/.local/share/hive-worker/secrets.json, 0600,
// {"<provider>": "<key>"}) keyed by the spec's provider name.

#include <nlohmann/json.hpp>

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

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
    bool    auto_approve = false;
    std::string system_prompt; // written to the shared volume, passed by path
    nlohmann::json mcp_servers = nlohmann::json::array();
    std::string image;         // "" -> kImageLatest (verbatim override wins)
    std::string type;          // Phase 35: "" -> standard, "gpu" -> kImageGpu + devices
    std::vector<std::string> disabled_skills;  // Phase 50: hub's per-skill gate
};

// Phase 35: explicit image wins; otherwise type selects the image.
// Inline here (not static in fleet.cpp) so tests/v2/test_fleet can
// verify the type -> image mapping without spawning anything.
inline std::string resolved_image(const AgentSpec& s) {
    if (!s.image.empty()) return s.image;
    return s.type == "gpu" ? kImageGpu : kImageLatest;
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
    /// down | starting | ready | acting | restarting
    std::string state() const;

private:
    void spawn_exec();      // fork + podman exec -i, reader+monitor threads
    void reader_loop(int fd);
    void monitor_loop();
    void kill_child();
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
    void spawn(const AgentSpec& spec);
    void stop(const std::string& agent_id);
    /// Roster delete: stop the brain AND destroy its container + env file
    /// + home dir (best-effort; agent row is already gone server-side).
    void remove(const std::string& agent_id);
    /// Route an op (task / approval_resp / cancel) to an agent.
    /// Returns false if the agent is not running here.
    bool send_to(const std::string& agent_id, const nlohmann::json& op);

    /// {agent_id: state} for heartbeats.
    nlohmann::json statuses() const;

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
