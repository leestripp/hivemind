#pragma once

// ChatService (HiveMind 2.0 Phase C): glues threads to agents. v2: turns
// run on WORKERS (hive-agent in containers) - this service is pure hub
// bookkeeping: post user messages, dispatch tasks via HubRouter, track
// live turn state until task_result arrives, fan agent replies into
// peer participants (generation-capped).

#include "hivemind/MessageHeader.h"

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
namespace hivemind { class DeliveryStore; struct Delivery; }

class ChatService {
public:
    ChatService(AgentStore& agents, ThreadStore& threads, HubRouter& hub,
                hivemind::DeliveryStore& deliveries)
        : agents_(agents), threads_(threads), hub_(hub),
          deliveries_(deliveries) { start_watchdog(); }
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
        // 2026-10-01 cancel-race fix: exactly ONE user-facing reply per
        // turn. complete_turn sets this under m_ before appending the
        // full body; flush_partial checks it so a racing cancel cannot
        // double-post (Bob DM incident — full body AND interrupted
        // partial both landed, user heard the reply twice via TTS).
        bool reply_posted = false;
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
    /// 2026-10-04 (Plan.md): `source` is the structured message header
    /// field (MessageHeader.h vocabulary) persisted on the delivery row.
    /// "user" (default) for UI posts, "voice-app" for the voice app,
    /// "routine:<name>" for routine fires — one path, three sources.
    std::string post_user_message(const std::string& thread_id,
                                  const std::string& body,
                                  const std::string& attachments_json = "[]",
                                  const std::string& source =
                                      std::string(hivemind::msg_source::kUser));

    /// Agent->agent async message: find-or-create the pair thread, post
    /// as sender_kind="agent", fan in to the target at generation 1.
    /// Phase 66d: when delivery_id_out is given it receives the durable
    /// delivery id (the message_status handle); "" when no wake was
    /// needed (fan-in suppression). A content-dedup hit returns the
    /// ORIGINAL message's thread + delivery id (2026-10-22, B5) so a
    /// retrying sender gets a working status handle.
    std::string post_agent_message(const std::string& sender_id,
                                   const std::string& target_id,
                                   const std::string& body,
                                   std::string* delivery_id_out = nullptr);

    /// Phase 66d — message_status surface. With a delivery id: that one
    /// delivery (empty vector when unknown/not the agent's). Without:
    /// the agent's ~10 most recent sends, newest first.
    std::vector<hivemind::Delivery> delivery_status(
        const std::string& agent_id, const std::string& delivery_id = "");
    /// True when the target's read high-water mark covers this
    /// delivery's source message (read receipt).
    bool delivery_is_read(const hivemind::Delivery& d);
    /// Phase 66e: admin observability (GET /api/deliveries) — raw list,
    /// newest first, optional agent (sender OR target) and state filters.
    /// Advance the HUMAN USER's read mark in a thread (Phase 66c).
    /// Keyed under the reserved id "user" — a row completely separate
    /// from every agent's, so the user viewing a thread in the client
    /// can never flip an agent-addressed delivery's read receipt
    /// (delivery_is_read consults only the target agent's mark).
    /// For messages addressed TO the user (user-inbox thread), this
    /// mark IS the "read by me" state. No-op on empty threads.
    void user_mark_read(const std::string& thread_id);
    /// Unread count for the human user: non-user messages newer than
    /// the "user" read mark. No mark yet → every non-user message
    /// counts (you genuinely haven't read them).
    int unread_for_user(const std::string& thread_id);
    /// Any principal's read mark in a thread ("user" or an agent id).
    std::optional<std::string> read_mark(const std::string& thread_id,
                                         const std::string& agent_id);

    std::vector<hivemind::Delivery> deliveries_list(
        const std::string& agent_id, const std::string& state, int limit);

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
    /// 2026-10-15 (Plan.md): an attach_file attachment shipped MID-TURN.
    /// Posts the file(s) as their own complete message immediately so
    /// they survive turn interrupts (watchdog/cancel) — attachments that
    /// only ride task_result die with the turn. NOT a turn boundary: no
    /// fan-in, no relay, no delivery/mute/breaker bookkeeping. The task
    /// may be unknown (hub restart) — the frame carries thread/agent.
    void post_turn_attachment(const std::string& task_id,
                              const std::string& thread_id,
                              const std::string& agent_id,
                              const std::string& attachments_json);
    /// Agent reported "ready" (spawn/reconnect): release one held wake.
    /// Recovery hook for wakes held across a worker restart.
    void on_agent_ready(const std::string& agent_id);
    /// Phase 52: post a dying turn's streamed partial into its thread so
    /// user-facing text survives cancel/timeout. No-op when empty or
    /// already flushed. Sets partial_flushed.
    void flush_partial(LiveTurn& t);
    /// 2026-10-22 (bug sweep B4): the "dispatch deferred (worker
    /// offline)" visibility notice — deduped against the thread's last
    /// message so an offline stretch posts it once, not once per wake.
    void post_dispatch_deferred_notice(const std::string& thread_id,
                                       const std::string& agent_name);
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

    /// True while the agent has any live (not done, not cancelling) turn
    /// in any thread. HubRouter defers grant-push respawns on this —
    /// restarting a busy brain kills the in-flight turn (2026-09-29).
    bool agent_busy(const std::string& agent_id);

    /// Shutdown: stop accepting new messages. (Turns are remote; the
    /// workers outlive us or get their own shutdown path.)
    void quiesce();

    /// Cancel every live turn in a thread.
    void cancel_thread(const std::string& thread_id);
    /// Phase 57: cancel every live turn owned by an agent, any thread.
    /// Returns the number of turns cancelled (0 = agent wasn't working).
    int cancel_agent_turns(const std::string& agent_id);

private:
    /// Phase 66b: fan_in is enqueue + try_dispatch. Every wake is a
    /// durable DeliveryStore row (queued -> dispatched -> done|error|
    /// dropped) that survives hub restarts; gates (mute, circuit
    /// breaker) are re-checked AT DISPATCH TIME and a refusal is
    /// recorded on the row with its reason. kind: "wake" | "relay".
    /// Returns the delivery id ("" when suppressed as a duplicate).
    std::string fan_in(const std::string& thread_id,
                const RosterAgent& agent,
                const std::string& sender_label,
                const std::string& source, const std::string& body,
                int generation, const std::string& sender_id = "",
                const std::string& source_message_id = "",
                const std::string& kind = "wake");
    /// Dispatch the oldest queued delivery for an IDLE agent — ONE per
    /// call (the dispatched turn re-busies the agent; the next row
    /// drains on that turn's completion / agent-ready / watchdog).
    /// Gate refusals mark the row dropped with the reason.
    void try_dispatch(const std::string& agent_id);

    // ── Origin-linked relay (2026-09-23; durable since Phase 66b) ────
    // Chief asks Honey via message_agent ON THE USER'S BEHALF: Honey's
    // reply lands in the pair thread, invisible in the thread the user
    // is watching. Fix: when message_agent is sent DURING a live turn,
    // record a durable one-shot link (DeliveryStore.relay_links) from
    // the pair thread to the requester's CURRENT turn thread — any kind
    // (user DM, routine check-in, group; the 21:00 routine blind spot
    // of 2026-09-26). When the peer's reply posts in the pair thread,
    // mirror it into the home thread and wake the requester there —
    // and the requester's ordinary pair-thread fan-in is SUPPRESSED
    // for that reply (single wake path; kills the 09-25 duplicates).
    // Links expire (HIVEMIND_RELAY_TTL_S, def 900) via watchdog sweep.
    void record_relay(const std::string& sender_id,
                      const std::string& pair_tid);
    /// Returns the requester id the relay fired to ("" = no relay).
    std::string maybe_fire_relay(const std::string& thread_id,
                                 const std::string& replier_id,
                                 const std::string& replier_name,
                                 const std::string& body, int generation);
    static std::chrono::seconds relay_ttl();

    // ── Durable wake queue (Phase 66b) ───────────────────────────────
    // The in-memory busy-relay queue (2026-09-24) and busy-receiver hold
    // queue (2026-09-25) are RETIRED: both existed only because wakes
    // were volatile. Every wake is now a DeliveryStore row — busy
    // receivers simply stay 'queued' until try_dispatch finds the agent
    // idle (complete_turn / on_agent_ready / watchdog hooks), hub
    // restarts requeue orphans on boot, and wake TTL expiry is a visible
    // 'expired' state instead of a silent drop.
    /// True while the agent has any live (not done, not cancelling)
    /// turn in any thread. Caller holds m_.
    bool agent_busy_locked(const std::string& agent_id) const;
    static int wake_ttl_s();   // env HIVEMIND_WAKE_TTL_S, def 86400

    // ── agent_msg content dedup (2026-09-25) ─────────────────────────
    // Double-POST protection ONLY (Phase 66b): a sender whose tool call
    // is slow retries message_agent and the identical body would post
    // twice. Wake-path duplication is handled by DeliveryStore
    // idempotency (has_live_wake per target+source message). Identical
    // (sender, target, body) within HIVEMIND_MSG_DEDUP_S (default 30)
    // is a retry — drop the repeat.
    struct RecentMsg {
        std::string sender, target, body;
        std::string thread_id;      // B5: pair thread (dedup returns it)
        std::string delivery_id;    // B5: original wake (dedup returns it)
        std::chrono::steady_clock::time_point at;
    };
    std::deque<RecentMsg> recent_agent_msgs_;   // guarded by m_, FIFO
    static int dedup_window_s();   // env HIVEMIND_MSG_DEDUP_S, def 30

    AgentStore& agents_;
    ThreadStore& threads_;
    HubRouter& hub_;
    hivemind::DeliveryStore& deliveries_;   // Phase 66b durable queue

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
