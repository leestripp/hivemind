#pragma once

// ChatService (HiveMind 2.0 Phase C): glues threads to agents. v2: turns
// run on WORKERS (hive-agent in containers) - this service is pure hub
// bookkeeping: post user messages, dispatch tasks via HubRouter, track
// live turn state until task_result arrives, fan agent replies into
// peer participants (generation-capped).

#include <atomic>
#include <memory>
#include <mutex>
#include <chrono>
#include <string>
#include <thread>
#include <unordered_set>
#include <unordered_map>
#include <vector>

class AgentStore;
class ThreadStore;
class HubRouter;
struct RosterAgent;

class ChatService {
public:
    ChatService(AgentStore& agents, ThreadStore& threads, HubRouter& hub)
        : agents_(agents), threads_(threads), hub_(hub) { start_watchdog(); }
    ~ChatService();

    struct LiveTurn {
        std::string task_id;
        std::string thread_id;
        std::string agent_id;
        std::string agent_name;
        int         generation = 0;   // agent->agent fan-in depth
        std::string partial;          // streamed so far (task_delta)
        std::string error;            // non-empty if the turn failed
        std::string worker;           // placement, for the exec audit row
        std::chrono::steady_clock::time_point started =
            std::chrono::steady_clock::now();  // turn start, for avg_ms
        bool done = false;
        bool cancelling = false;
        // When cancelling began — the watchdog retires a cancelling turn
        // after a grace period if the worker never confirms (dropped task
        // frame / dead worker / idle brain → no task_result). Without this
        // a cancelled-but-unconfirmed turn wedges busy forever and the
        // watchdog SKIPS it (Forge incident, 2026-09-07).
        std::chrono::steady_clock::time_point cancel_started{};
    };

    /// Record a user message and dispatch a task to every agent
    /// participant of the thread. Returns the user message id.
    /// Throws std::invalid_argument if the thread does not exist.
    std::string post_user_message(const std::string& thread_id,
                                  const std::string& body,
                                  const std::string& attachments_json = "[]");

    /// Agent->agent async message: find-or-create the pair thread, post
    /// as sender_kind="agent", fan in to the target at generation 1.
    std::string post_agent_message(const std::string& sender_id,
                                   const std::string& target_id,
                                   const std::string& body);

    /// Proactive nudge (gap analysis): sender_kind="system", fanned in.
    std::string post_nudge(const std::string& thread_id,
                           const std::string& body);

    // -- HubRouter callbacks (worker channel events) --------------------
    void append_delta(const std::string& task_id, const std::string& text);
    void complete_turn(const std::string& task_id, const std::string& body,
                       const std::string& error,
                       const std::string& attachments_json = "[]",
                       // Restart recovery (Phase 39 H2): hub restarts wipe
                       // LiveTurns, so a completing task is unknown. The
                       // frame still carries thread_id/agent_id — post the
                       // reply to the thread instead of dropping it.
                       const std::string& frame_thread_id = "",
                       const std::string& frame_agent_id = "");

    /// Snapshot of in-progress (and just-finished) turns for a thread.
    std::vector<LiveTurn> activity(const std::string& thread_id);
    /// True while any turn for this thread is still running.
    bool busy(const std::string& thread_id);
    /// Latest live turn per agent across ALL threads (roster status).
    std::unordered_map<std::string, LiveTurn> agent_activity();

    /// Shutdown: stop accepting new messages. (Turns are remote; the
    /// workers outlive us or get their own shutdown path.)
    void quiesce();

    /// Cancel every live turn in a thread.
    void cancel_thread(const std::string& thread_id);

private:
    void fan_in(const std::string& thread_id, const RosterAgent& agent,
                const std::string& sender_label, const std::string& body,
                int generation);

    AgentStore& agents_;
    ThreadStore& threads_;
    HubRouter& hub_;

    std::mutex m_;
    std::unordered_map<std::string, std::vector<std::shared_ptr<LiveTurn>>> live_;
    std::unordered_map<std::string, std::shared_ptr<LiveTurn>> by_task_;
    std::atomic<bool> stopping_{false};

    // Phase 39 H3 — turn watchdog. hive-agent runs tasks serially per
    // agent, so one hung turn strands every later task to that agent;
    // hub-side there was NO timeout at all (a wedged worker task sat in
    // by_task_ forever, 2026-09-03 incident). The watchdog sweeps every
    // 60s and force-cancels turns older than HIVEMIND_TURN_TIMEOUT_S
    // (default 1200; 0 disables). cancelled_tasks_ tombstones tell
    // complete_turn to silently drop a late result for a cancelled task
    // instead of taking the H2 restart-recovery path.
    std::thread watchdog_;
    std::unordered_set<std::string> cancelled_tasks_;
    void start_watchdog();
    void watchdog_loop();
    static int turn_timeout_s();   // env HIVEMIND_TURN_TIMEOUT_S, def 1200
    // Grace for a cancel to be confirmed by the worker before the watchdog
    // force-retires the turn. env HIVEMIND_CANCEL_GRACE_S, def 90.
    static int cancel_grace_s();

    static constexpr std::size_t kMaxLivePerThread = 32;
    static constexpr int kMaxFanInGenerations = 3;
};
