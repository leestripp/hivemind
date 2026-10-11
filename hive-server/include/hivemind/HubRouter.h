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
#include <unordered_set>
#include <vector>

#include <nlohmann/json.hpp>

class AgentStore;
class RoutineStore;
class ThreadStore;
namespace hivemind { class SecretStore; class ShareStore; }
class WorkerChannel;
class ApprovalStore;
class ChatService;
struct RosterAgent;
class LlmStore;

/// Recent User Inbox messages as a task-frame payload: sender display
/// name, text, created_at — chronological, capped at kUserInboxWindow.
/// Agents read it via the read_user_inbox tool (the inbox was write-only
/// for agents before this; they could message_user but never read back).
nlohmann::json user_inbox_snapshot(ThreadStore& threads,
                                   AgentStore& agents,
                                   int limit = 10);

/// secrets_create op core (unit-testable): validate + store + auto-grant
/// to the creating agent. Returns {"ok":true,"created":name} or
/// {"ok":false,"error":...}. The VALUE never appears in the result or
/// any log line. `type` payload shapes mirror SecretRoutes:
///   api_key {"value"} | login {"username","password","url"} | note {"text"}
nlohmann::json secret_create_apply(hivemind::SecretStore& secrets,
                                   const std::string& name,
                                   const std::string& type,
                                   const nlohmann::json& payload,
                                   const std::string& agent_id);

class HubRouter {
public:
    HubRouter(AgentStore& agents, ThreadStore& threads,
              WorkerChannel& channel, ApprovalStore& approvals,
              RoutineStore& routines, hivemind::SecretStore& secrets);
    virtual ~HubRouter() = default;

    void set_chat_service(ChatService* cs) { chat_ = cs; }

    /// Phase 43: hub-owned LLM registry — dispatch resolves the agent's
    /// model here and injects the full provider config into the frame.
    void set_llm_store(LlmStore* s) { llm_ = s; }

    /// 2026-10-11 (Plan.md "Agent File Sharing"): share metadata store
    /// for the share_file tool bridge. Setter-injected like chat_/llm_
    /// so existing constructions (tests) keep compiling.
    void set_share_store(hivemind::ShareStore* s) { shares_ = s; }

    /// Dispatch a task to the agent's worker. Returns the task id, or ""
    /// when the worker is offline (caller marks the live turn failed).
    /// virtual: tests subclass with a capturing mock (no worker needed).
    virtual std::string dispatch(const RosterAgent& agent,
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
    /// Phase 66d: confirmed message_agent (msg_send_req -> msg_send_resp
    /// with the durable delivery id) and the message_status query
    /// surface (msg_status_req -> msg_status_resp).
    void handle_msg_send_req(const std::string& worker,
                             const std::string& agent_id,
                             const nlohmann::json& frame);
    void handle_msg_status_req(const std::string& worker,
                               const std::string& agent_id,
                               const nlohmann::json& frame);
    void handle_secret_req(const std::string& worker,
                           const std::string& agent_id,
                           const nlohmann::json& frame);
    /// 2026-10-11: share_file tool bridge (Plan.md "Agent File Sharing").
    /// create/list/fetch/revoke mirror the msg_send_req reply style;
    /// share_copy_ack completes a same-worker fetch (share_local_copy op).
    void handle_share_create_req(const std::string& worker,
                                 const std::string& agent_id,
                                 const nlohmann::json& frame);
    void handle_share_list_req(const std::string& worker,
                               const std::string& agent_id,
                               const nlohmann::json& frame);
    void handle_share_fetch_req(const std::string& worker,
                                const std::string& agent_id,
                                const nlohmann::json& frame);
    void handle_share_revoke_req(const std::string& worker,
                                 const std::string& agent_id,
                                 const nlohmann::json& frame);
    void handle_share_copy_ack(const std::string& worker,
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
    /// Grant/model-change respawn push (Phase 67.2): spawn frames restart
    /// the brain on spec change, so pushing one while the agent is
    /// MID-TURN kills the turn and wedges the hub-side LiveTurn (no
    /// task_result ever arrives — the 2026-09-29 "agents stuck until you
    /// tap stop" regression). When the agent is busy the push is deferred
    /// and flushed on turn completion / heartbeat; idle agents spawn
    /// immediately. Never throws (logs instead) — safe for route
    /// callbacks.
    void push_spawn(const std::string& agent_id);
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
    hivemind::SecretStore& secrets_;
    ChatService* chat_ = nullptr;
    LlmStore* llm_ = nullptr;
    hivemind::ShareStore* shares_ = nullptr;

    mutable std::mutex m_;
    struct Task { std::string agent_id; std::string worker; };
    std::unordered_map<std::string, Task> tasks_;       // task_id -> owner
    std::unordered_map<std::string, std::string> states_;  // agent -> state
    // approval id -> (worker, agent_id) for the response relay
    struct PendAppr { std::string worker; std::string agent_id;
                      std::string req_id; };  // req_id = worker-side id
    std::unordered_map<std::string, PendAppr> pend_appr_;
    // Agents whose grant-push spawn frame is waiting for their current
    // turn to finish (flush_pending_spawns: task_result + heartbeat).
    std::unordered_set<std::string> pending_spawn_;
    // Same-worker share fetches waiting on the worker's share_copy_ack:
    // share_id -> who asked (response relay target).
    struct PendFetch { std::string worker; std::string agent_id;
                       std::string req_id; };
    std::unordered_map<std::string, PendFetch> pend_fetch_;
    void flush_pending_spawns();
    uint64_t next_task_ = 1;
};
