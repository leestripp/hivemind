#pragma once

#include "hivemind/AgentStore.h"
#include "hivemind/ThreadStore.h"
#include "hivemind/RoutineStore.h"
#include "hivemind/ApprovalStore.h"

#include <daedalus.h>

#include <condition_variable>
#include <deque>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <set>
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
class ChatService;
class LLMStore;

/// Sanitize a roster display name into a valid daedalus agent_name
/// (also determines the on-disk `.daedalus/daedalus-<name>` namespace).
std::string sanitize_agent_name(std::string name);

class AgentHost {
public:
    AgentHost(RosterAgent info, std::filesystem::path dir,
              AgentStore& agents, ApprovalStore& approvals,
              AgentRegistry& registry, LLMStore& llm,
              RoutineStore& routines, ThreadStore& threads);
    ~AgentHost();

    AgentHost(const AgentHost&) = delete;
    AgentHost& operator=(const AgentHost&) = delete;

    const RosterAgent& info() const { return info_; }

    /// Current daedalus agent state as a lowercase string ("acting",
    /// "planning", …; "idle" when no agent is constructed). Thread-safe:
    /// Agent::state() is const/noexcept and daedalus hosts are expected to
    /// render it from UI threads.
    std::string state_name() const noexcept;

    /// Evicted hosts are marked dead (new turns rejected) but never
    /// destroyed mid-process — see AgentRegistry's graveyard rule.
    void mark_dead() noexcept { dead_ = true; }
    bool dead() const noexcept { return dead_; }

    /// One queued turn (Phase 11.1). enqueue() is non-blocking: the prompt
    /// is pushed to the agent's queue and a driver thread drains it with
    /// run_one_turn() — daedalus's own message model instead of a
    /// synchronous call that holds every mutex for the whole LLM round.
    struct PendingTurn {
        std::string prompt;
        std::function<void(std::string_view)> on_delta;   // streamed text
        // (user-facing output, error-or-empty) — exactly once per turn
        std::function<void(std::string, std::string)> on_done;
    };

    /// Queue a turn. Throws std::runtime_error if the host is dead/stopping.
    void enqueue(PendingTurn turn);

    /// Cancel the in-flight turn (daedalus Agent::cancel — stop_token).
    /// Safe to call when idle.
    void cancel() noexcept;

    /// Phase 8 idle eviction: activity tracking. touch() on enqueue and at
    /// every turn completion; busy() while a turn is queued or in flight —
    /// the registry never evicts a busy host.
    void touch() noexcept {
        last_active_.store(std::chrono::steady_clock::now(),
                           std::memory_order_relaxed);
    }
    std::chrono::steady_clock::duration idle_for() const noexcept {
        return std::chrono::steady_clock::now() -
               last_active_.load(std::memory_order_relaxed);
    }
    bool busy() const noexcept {
        if (in_flight_.load(std::memory_order_relaxed) > 0) return true;
        std::lock_guard<std::mutex> lk(queue_m_);
        return !queue_.empty();
    }

    /// Run one full turn synchronously (compat wrapper over enqueue()).
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
    /// Phase 16: rule-based auto-review ahead of the human queue.
    /// A matching approval_rules row short-circuits (recorded as
    /// auto_allowed/auto_denied); no match → create + wait as before.
    bool request_approval(const std::string& kind, const std::string& payload);

    void ensure_agent();  // lazy construction (caller holds turn_mtx_)
    void driver_loop();   // drains queue_ (one thread per host, on demand)
    void run_one(PendingTurn& turn);   // one submit + run_one_turn cycle

    RosterAgent info_;
    std::filesystem::path dir_;
    std::atomic<bool> dead_{false};
    AgentStore& agents_;
    ApprovalStore& approvals_;
    AgentRegistry& registry_;
    LLMStore& llm_;
    RoutineStore& routines_;
    ThreadStore& threads_;
    bool containerized_ = false;            // workspace_mode != "host"
    std::mutex turn_mtx_;                   // one turn at a time per agent
    std::shared_ptr<ProxySink> proxy_sink_; // stable sink owned by the Agent
    std::unique_ptr<daedalus::Agent> agent_;

    // Phase 8 idle eviction state
    std::atomic<std::chrono::steady_clock::time_point> last_active_{
        std::chrono::steady_clock::now()};
    std::atomic<int> in_flight_{0};

    // Phase 11.1 queue state
    mutable std::mutex queue_m_;
    std::condition_variable queue_cv_;
    std::deque<PendingTurn> queue_;
    bool driver_running_ = false;           // guarded by queue_m_
    bool stopping_ = false;                 // guarded by queue_m_
};

/// AgentRegistry — process-wide map of live AgentHosts, keyed by agent id.
///
/// OWNERSHIP RULE (Phase 10, hardened): the registry is the SOLE owner
/// (unique_ptr). Hosts are NEVER destroyed mid-process — evict() marks a
/// host dead and moves it to the graveyard_, which is torn down only by
/// shutdown_all() at process exit (after ChatService::quiesce() has drained
/// every turn). References returned by get_or_create() are therefore valid
/// for the whole process lifetime. No refcounting anywhere.
class AgentRegistry {
public:
    AgentRegistry(AgentStore& agents, ApprovalStore& approvals,
                  LLMStore& llm, RoutineStore& routines, ThreadStore& threads)
        : agents_(agents), approvals_(approvals), llm_(llm),
          routines_(routines), threads_(threads) {}

    /// Get the live host for a roster agent, constructing it on first use.
    /// An evicted (dead) host is replaced with a fresh one.
    AgentHost& get_or_create(const RosterAgent& agent);

    /// Set once from main() after ChatService is constructed (it depends on
    /// the registry, so the registry can't take it in the constructor).
    /// Read by MessageAgentTool at execute() time — always set by then.
    void set_chat_service(ChatService* cs) { chat_service_ = cs; }
    ChatService* chat_service() const { return chat_service_; }

    /// Live host for id, or nullptr (never constructs). For cancel paths.
    AgentHost* find(const std::string& agent_id);

    /// Live agent's state name, or "idle" when no host is live.
    std::string agent_state(const std::string& agent_id);

    /// Mark the host dead and retire it to the graveyard (stays allocated
    /// until shutdown_all). In-flight turns finish safely; new turns on the
    /// dead host are rejected.
    void evict(const std::string& agent_id);

    /// Phase 8: evict hosts idle longer than timeout (skips busy hosts).
    void evict_idle(std::chrono::steady_clock::duration timeout);

    /// Shut down (persist history) and destroy every host, live and
    /// graveyard. Call after ChatService::quiesce(), from the server's
    /// signal-shutdown path.
    void shutdown_all();

private:
    ChatService* chat_service_ = nullptr;
    AgentStore& agents_;
    ApprovalStore& approvals_;
    LLMStore& llm_;
    RoutineStore& routines_;
    ThreadStore& threads_;
    std::mutex m_;
    std::unordered_map<std::string, std::unique_ptr<AgentHost>> hosts_;
    std::vector<std::unique_ptr<AgentHost>> graveyard_;  // dead, not freed
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
        std::string partial;      // streamed so far
        std::string error;        // non-empty if the turn failed
        bool done = false;
        bool cancelling = false;  // cancel requested (Phase 11.3)
    };

    /// Record a user message and kick off a background turn for every agent
    /// participant of the thread. Returns the user message id.
    /// Throws std::invalid_argument if the thread does not exist.
    /// `attachments_json` (Phase 18): JSON array of {"name","path",...}
    /// where path is the container-visible /workspace/... location — the
    /// agent prompt gets an explicit reference list so the LLM knows the
    /// files exist and where to read them.
    std::string post_user_message(const std::string& thread_id,
                                  const std::string& body,
                                  const std::string& attachments_json = "[]");

    /// Agent→agent async message (message_agent tool): find-or-create the
    /// pair thread (kind "group", exactly these two agents), post the body
    /// as sender_kind="agent", and fan it in to the target at generation 1
    /// (replies fan back, capped by kMaxFanInGenerations). Returns the
    /// thread id; throws std::invalid_argument on unknown agents.
    std::string post_agent_message(const std::string& sender_id,
                                   const std::string& target_id,
                                   const std::string& body);

    /// Proactive nudge (gap analysis): post a sender_kind="system" message
    /// into a thread and fan it in to agent participants like a user
    /// message. Used by the scheduler when a thread's latest message is an
    /// unanswered user message — the system message itself then excludes
    /// the thread from further nudges until the user speaks again.
    /// Returns the message id; throws if the thread does not exist.
    std::string post_nudge(const std::string& thread_id,
                           const std::string& body);

    /// Snapshot of in-progress (and just-finished) turns for a thread.
    std::vector<LiveTurn> activity(const std::string& thread_id);

    /// True while any turn for this thread is still running.
    bool busy(const std::string& thread_id);

    /// Latest live turn per agent across ALL threads (web UI roster status).
    /// Only agents with a not-yet-done turn appear in the map.
    std::unordered_map<std::string, LiveTurn> agent_activity();

    /// Shutdown support (Phase 10): stop accepting new messages and wait
    /// until every in-flight turn thread has fully finished (including its
    /// final message write). Call before the stores go out of scope.
    void quiesce();

    /// Cancel every live turn in a thread (Phase 11.3).
    void cancel_thread(const std::string& thread_id);

private:
    /// Fan a message into one agent's queue (Phase 11.1/11.2). The agent's
    /// daedalus conversation (persisted across restarts) IS the context —
    /// each turn carries only the new message. generation caps agent→agent
    /// fan-in loops.
    void fan_in(const std::string& thread_id, const RosterAgent& agent,
                const std::string& sender_label, const std::string& body,
                int generation);

    AgentStore& agents_;
    ThreadStore& threads_;
    AgentRegistry& registry_;

    std::mutex m_;
    std::unordered_map<std::string, std::vector<std::shared_ptr<LiveTurn>>> live_;
    std::atomic<int> active_turns_{0};
    std::atomic<bool> stopping_{false};
    static constexpr std::size_t kMaxLivePerThread = 32;
    static constexpr int kMaxFanInGenerations = 3;  // agent→agent loop cap
};
