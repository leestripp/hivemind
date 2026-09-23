#pragma once

// ChatService (HiveMind 2.0 Phase C): glues threads to agents. v2: turns
// run on WORKERS (hive-agent in containers) - this service is pure hub
// bookkeeping: post user messages, dispatch tasks via HubRouter, track
// live turn state until task_result arrives, fan agent replies into
// peer participants (generation-capped).

#include <atomic>
#include <deque>
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
        // Phase 52: partial already posted to the thread (cancel /
        // watchdog flush) — guards against double-posting.
        bool partial_flushed = false;
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

    /// Agent -> human direct message (Phase 44): posts to the singleton
    /// User Inbox thread (kUserThreadId) as sender_kind="agent". NO
    /// fan_in, NO dispatch — the inbox has zero agent participants, so
    /// nothing an agent writes there can trigger another turn. This is
    /// the loop-safe channel for agent-to-user output.
    /// Throws std::invalid_argument for an unknown sender agent.
    std::string post_user_inbox_message(const std::string& sender_id,
                                        const std::string& body);

    /// Proactive nudge (gap analysis): sender_kind="system", fanned in.
    std::string post_nudge(const std::string& thread_id,
                           const std::string& body);

    // -- HubRouter callbacks (worker channel events) --------------------
    void append_delta(const std::string& task_id, const std::string& text);
    /// Phase 52: post a dying turn's streamed partial into its thread so
    /// user-facing text survives cancel/timeout. No-op when empty or
    /// already flushed. Sets partial_flushed.
    void flush_partial(LiveTurn& t);
    void complete_turn(const std::string& task_id, const std::string& body,
                       const std::string& error,
                       const std::string& attachments_json = "[]",
                       // Restart recovery (Phase 39 H2): hub restarts wipe
                       // LiveTurns, so a completing task is unknown. The
                       // frame still carries thread_id/agent_id — post the
                       // reply to the thread instead of dropping it.
                       const std::string& frame_thread_id = "",
                       const std::string& frame_agent_id = "",
                       // Phase 64: per-turn agent stats (tokens, cache,
                       // iterations) — stored on the posted message.
                       const std::string& stats_json = "");

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
    /// Phase 57: cancel every live turn owned by an agent, any thread.
    /// Returns the number of turns cancelled (0 = agent wasn't working).
    int cancel_agent_turns(const std::string& agent_id);

private:
    void fan_in(const std::string& thread_id, const RosterAgent& agent,
                const std::string& sender_label, const std::string& body,
                int generation);

    // ── Origin-linked relay (2026-09-23) ─────────────────────────────
    // Chief asks Honey via message_agent ON THE USER'S BEHALF: Honey's
    // reply lands in the pair thread, invisible in the user's DM with
    // Chief, so Chief "never tells me what Honey said". Fix: when
    // message_agent is sent DURING a live turn in a user DM thread,
    // record a one-shot link; when the peer's substantive reply posts in
    // the pair thread, mirror it into the user DM and wake the requester
    // there to frame it. Agent chatter (link only from user-DM turns)
    // never mirrors; links expire (HIVEMIND_RELAY_TTL_S, def 900).
    struct RelayLink {
        std::string pair_thread_id;
        std::string requester_id;
        std::string user_thread_id;
        std::chrono::steady_clock::time_point created;
        bool consumed = false;
    };
    std::vector<RelayLink> relay_links_;   // guarded by m_
    void record_relay(const std::string& sender_id,
                      const std::string& pair_tid);
    void maybe_fire_relay(const std::string& thread_id,
                          const std::string& replier_id,
                          const std::string& replier_name,
                          const std::string& body, int generation);
    static std::chrono::seconds relay_ttl();

    // ── Busy-agent relay queue (2026-09-24) ──────────────────────────
    // The relay fired while the requester was still busy: the wake task
    // queued agent-side, but the hub-side LiveTurn aged from DISPATCH —
    // a requester busy past HIVEMIND_TURN_TIMEOUT_S had the queued wake
    // force-retired by the watchdog and its result tombstoned, so the
    // user saw the peer's mirrored reply but the requester never framed
    // it. Fix: mirror immediately (user sees the raw reply), but hold
    // the WAKE here while the requester has any live turn; drain when it
    // goes idle (end of complete_turn / watchdog retirement). Same TTL
    // as relay links.
    struct PendingRelay {
        std::string requester_id;
        std::string user_thread_id;
        std::string replier_id;
        std::string replier_name;
        std::string body;
        int generation = 0;
        std::chrono::steady_clock::time_point created;
    };
    std::vector<PendingRelay> relay_queue_;   // guarded by m_, FIFO
    /// True while the agent has any live (not done, not cancelling)
    /// turn in any thread. Caller holds m_.
    bool agent_busy_locked(const std::string& agent_id) const;
    /// Fire queued wakes for an idle agent (TTL-reaped first). Safe to
    /// call without holding m_; ThreadStore/fan_in happen outside it.
    void drain_relay_queue(const std::string& agent_id);

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

    // ── Phase 65a — Stop = MUTE ────────────────────────────────────────
    // The 2026-09-16 storm proved cancel-per-turn useless against fan-in:
    // peers mid-turn completed and immediately re-dispatched to the
    // just-cancelled agent. Cancel now also sets a mute deadline; fan_in
    // (generation > 0) drops tasks for muted agents/threads. A user
    // message in the thread lifts all its mutes; otherwise they expire.
    std::unordered_map<std::string, std::chrono::steady_clock::time_point>
        muted_agents_;    // agent_id  → muted until
    std::unordered_map<std::string, std::chrono::steady_clock::time_point>
        muted_threads_;   // thread_id → muted until
    bool is_muted_locked(const std::string& thread_id,
                         const std::string& agent_id);  // lazy expiry
    void mute_agent_locked(const std::string& agent_id);
    void mute_thread_locked(const std::string& thread_id);
    /// Lift thread mute + participant agent mutes + breaker reset.
    /// Caller passes the participant ids so no ThreadStore call happens
    /// under m_ (Phase 52 lock ordering: never call into ThreadStore
    /// while holding m_).
    void unmute_thread_locked(const std::string& thread_id,
                              const std::vector<std::string>& agent_ids);
    static int mute_s();   // env HIVEMIND_MUTE_S, def 600

    // ── Phase 65b — thread circuit breaker ─────────────────────────────
    // Sliding window over agent-triggered fan-ins (generation > 0). Trip
    // at HIVEMIND_CB_MAX_TURNS within HIVEMIND_CB_WINDOW_S: fan_in drops
    // and ONE system message is posted; a user message resets.
    struct ThreadThrottle {
        std::deque<std::chrono::steady_clock::time_point> fanins;
        bool tripped = false;
        bool announced = false;   // post the trip message only once
    };
    std::unordered_map<std::string, ThreadThrottle> throttle_;
    /// Returns false when the breaker is tripped. Sets *just_tripped on
    /// the transition so the caller can post the system message unlocked.
    bool circuit_allow_locked(const std::string& thread_id,
                              bool* just_tripped);
    static int cb_max_turns();   // env HIVEMIND_CB_MAX_TURNS, def 20
    static int cb_window_s();    // env HIVEMIND_CB_WINDOW_S, def 300

    static constexpr std::size_t kMaxLivePerThread = 32;
    static constexpr int kMaxFanInGenerations = 3;
};
