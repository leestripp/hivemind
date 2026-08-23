#pragma once

#include "hivemind/AgentStore.h"
#include "hivemind/ThreadStore.h"
#include "hivemind/ApprovalStore.h"

#include <daedalus.h>

#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

/**
 * AgentHost — a long-lived daedalus::Agent backing one roster agent
 * (HiveMind 2.0, Plan.md Phase 1).
 *
 * Persistence: the roster agent's directory
 * (~/.local/share/hivemind/agents/<id>/) is used as daedalus project_root and
 * the roster name as AgentConfig::agent_name, so daedalus keeps messages.json
 * / memory.json per agent under `<dir>/.daedalus-<name>/` — context survives
 * server restarts with no extra code on our side.
 *
 * Concurrency: daedalus v0.9.6 shares global state across Agent instances
 * (see AgentSerialization.h). The Agent is constructed once and kept alive;
 * every turn runs under agent_exec_mutex(). Correct, not concurrent —
 * parallel agent execution is a daedalus upgrade question, not our bug.
 */
class ProxySink;  // forwards AgentSink callbacks to a per-turn target
class AgentRegistry;

class AgentHost {
public:
    AgentHost(RosterAgent info, std::filesystem::path dir,
              AgentStore& agents, ApprovalStore& approvals,
              AgentRegistry& registry);
    ~AgentHost();

    AgentHost(const AgentHost&) = delete;
    AgentHost& operator=(const AgentHost&) = delete;

    const RosterAgent& info() const { return info_; }

    /// Run one full turn. `on_delta` fires with streamed assistant text
    /// (possibly from a background thread inside daedalus). Returns the
    /// complete assistant output. Throws std::runtime_error on agent errors.
    std::string run_turn(const std::string& prompt,
                         std::function<void(std::string_view)> on_delta);

    /// Graceful shutdown: daedalus only persists conversation history
    /// (messages.json) from LLMClient::cleanup(), which runs exclusively
    /// inside Agent::shutdown() — ~Agent does NOT call it. Hosts must
    /// shutdown() explicitly or history is lost. Safe to call when the
    /// agent was never started. After shutdown the next run_turn()
    /// re-initializes (and daedalus restores history via load()).
    void shutdown();

private:
    void ensure_agent();  // lazy construction (caller holds turn_mtx_)

    RosterAgent info_;
    std::filesystem::path dir_;
    AgentStore& agents_;
    ApprovalStore& approvals_;
    AgentRegistry& registry_;
    bool containerized_ = false;            // workspace_mode != "host"
    std::mutex turn_mtx_;                   // one turn at a time per agent
    std::shared_ptr<ProxySink> proxy_sink_; // stable sink owned by the Agent
    std::unique_ptr<daedalus::Agent> agent_;
};

/// AgentRegistry — process-wide map of live AgentHosts, keyed by agent id.
class AgentRegistry {
public:
    AgentRegistry(AgentStore& agents, ApprovalStore& approvals)
        : agents_(agents), approvals_(approvals) {}

    /// Get the live host for a roster agent, constructing it on first use.
    AgentHost& get_or_create(const RosterAgent& agent);

    /// Drop the live host (e.g. after the roster row is deleted or renamed).
    void evict(const std::string& agent_id);

    /// Gracefully shut down every live host (persists conversation history).
    /// Call from the server's signal-shutdown path.
    void shutdown_all();

private:
    AgentStore& agents_;
    ApprovalStore& approvals_;
    std::mutex m_;
    std::unordered_map<std::string, std::unique_ptr<AgentHost>> hosts_;
};

/// ChatService — glues threads to agents: posts user messages, runs agent
/// turns in the background, exposes live (in-progress) turn state for polling.
class ChatService {
public:
    ChatService(AgentStore& agents, ThreadStore& threads, AgentRegistry& registry)
        : agents_(agents), threads_(threads), registry_(registry) {}

    struct LiveTurn {
        std::string agent_id;
        std::string agent_name;
        std::string partial;   // streamed so far
        std::string error;     // non-empty if the turn failed
        bool done = false;
    };

    /// Record a user message and kick off a background turn for every agent
    /// participant of the thread. Returns the user message id.
    /// Throws std::invalid_argument if the thread does not exist.
    std::string post_user_message(const std::string& thread_id,
                                  const std::string& body);

    /// Snapshot of in-progress (and just-finished) turns for a thread.
    std::vector<LiveTurn> activity(const std::string& thread_id);

    /// True while any turn for this thread is still running.
    bool busy(const std::string& thread_id);

private:
    AgentStore& agents_;
    ThreadStore& threads_;
    AgentRegistry& registry_;

    std::mutex m_;
    std::unordered_map<std::string, std::vector<std::shared_ptr<LiveTurn>>> live_;
};
