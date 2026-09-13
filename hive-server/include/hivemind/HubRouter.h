#pragma once

// HubRouter (HiveMind 2.0 Phase C): the server is a PURE HUB. It owns no
// brains; it routes between roster/threads, workers, and the client.
//
//   thread message -> task op -> owning worker (hive-agent in container)
//   worker task_delta   -> ChatService live-turn partial (client polls)
//   worker task_result  -> ChatService complete_turn -> thread message
//   worker approval_req -> ApprovalStore (client decides) -> approval_resp
//   worker agent_msg    -> ChatService::post_agent_message (agent->agent)
//   heartbeat agents{}  -> presence map (roster online/state)
//   worker online       -> reconcile: agent_spawn for roster agents homed
//                          there (idempotent - worker dedups by agent id)
//
// Credentials never live here: agent_spawn carries model/provider NAMES;
// the worker resolves them from its own models.json + secrets.json.

#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>

#include <nlohmann/json.hpp>

class AgentStore;
class RoutineStore;
class ThreadStore;
class WorkerChannel;
class ApprovalStore;
class ChatService;
struct RosterAgent;
class LlmStore;

class HubRouter {
public:
    HubRouter(AgentStore& agents, ThreadStore& threads,
              WorkerChannel& channel, ApprovalStore& approvals,
              RoutineStore& routines);
    ~HubRouter() = default;

    void set_chat_service(ChatService* cs) { chat_ = cs; }

    /// Phase 43: hub-owned LLM registry — dispatch resolves the agent's
    /// model here and injects the full provider config into the frame.
    void set_llm_store(LlmStore* s) { llm_ = s; }

    /// Dispatch a task to the agent's worker. Returns the task id, or ""
    /// when the worker is offline (caller marks the live turn failed).
    std::string dispatch(const RosterAgent& agent,
                         const std::string& thread_id,
                         const std::string& text);

    /// Send a cancel for every in-flight task of (thread_id) / agent.
    void cancel_agent(const RosterAgent& agent, const std::string& thread_id);

    /// Worker channel frame handler - wire to WorkerChannel::on_frame.
    void on_worker_frame(const std::string& worker,
                         const nlohmann::json& frame);

    /// Phase 53: agent routine tools — apply a routine_req against
    /// RoutineStore (mirroring RoutineRoutes semantics, ownership-checked
    /// to the requesting agent) and reply routine_resp to the worker.
    void handle_routine_req(const std::string& worker,
                            const std::string& agent_id,
                            const nlohmann::json& frame);

    /// WorkerChannel::on_heartbeat hook: per-agent states map.
    void on_heartbeat(const std::string& worker,
                      const nlohmann::json& agents);

    /// WorkerChannel::on_worker_online hook: spawn roster agents homed
    /// on this worker (idempotent server-side too: tracks spawned ids).
    void reconcile_worker(const std::string& worker);

    /// Roster delete/reset: stop the agent on its owning worker (the
    /// container + brain volume persist; a later spawn resumes it).
    void stop_agent(const RosterAgent& agent);
    /// Spawn one agent on its worker; throws if the worker is offline.
    void spawn_agent(const RosterAgent& agent);
    /// Roster delete: worker destroys the container + local state too.
    void remove_agent(const RosterAgent& agent);

    /// Build the agent_spawn op for a roster agent.
    nlohmann::json spawn_frame(const RosterAgent& agent) const;

    /// ApprovalRoutes hook: the client decided a pending approval.
    /// Returns false when the id is unknown / not worker-owned.
    bool resolve_approval(const std::string& id, bool allow);

    /// Roster presence: last known state ("ready"/"acting"/"restarting"/
    /// "down") from heartbeats + agent_status frames. "" = unknown.
    std::string agent_state(const std::string& agent_id) const;

private:
    AgentStore& agents_;
    ThreadStore& threads_;
    WorkerChannel& channel_;
    ApprovalStore& approvals_;
    RoutineStore& routines_;
    ChatService* chat_ = nullptr;
    LlmStore* llm_ = nullptr;

    mutable std::mutex m_;
    struct Task { std::string agent_id; std::string worker; };
    std::unordered_map<std::string, Task> tasks_;       // task_id -> owner
    std::unordered_map<std::string, std::string> states_;  // agent -> state
    // approval id -> (worker, agent_id) for the response relay
    struct PendAppr { std::string worker; std::string agent_id;
                      std::string req_id; };  // req_id = worker-side id
    std::unordered_map<std::string, PendAppr> pend_appr_;
    uint64_t next_task_ = 1;
};
