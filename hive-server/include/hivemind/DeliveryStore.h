#pragma once

// =============================================================================
// DeliveryStore (Phase 66a): durable wake/relay queue + read high-water
// marks for agent messaging. See Plan.md (Phase 66) and docs/design.md.
//
// Messages persist in ThreadStore; the DELIVERY INTENTS (the wakes that
// tell an agent a message exists) historically lived in ChatService memory
// and died with every hub restart — the dominant silent-loss class of the
// 2026-09 relay incidents. This store makes every wake/relay a durable row
// with a state machine (queued -> dispatched -> done|error|dropped) that
// survives restarts (orphan requeue on boot), is idempotent per
// (target, source message), and gives agents a queryable status surface.
// =============================================================================

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace hivemind {

/// One wake/relay intent. State machine:
///   queued -> dispatched -> done | error | dropped | expired
struct Delivery {
    std::string id;
    std::string kind;              // "wake" | "relay"
    std::string thread_id;         // where the wake lands
    std::string target_agent_id;   // who is being woken
    std::string sender_agent_id;   // who caused it ("" for user/system)
    std::string sender_label;      // display label used in the prompt
    std::string source;            // controlled vocab (MessageHeader.h)
    std::string body;              // wake payload
    int         generation = 0;    // fan-in depth
    std::string state;             // queued|dispatched|done|error|dropped|expired
    std::string task_id;           // set at dispatch, kept for tracing
    std::string source_message_id; // thread message this wake points at
    std::string error;             // last error / drop reason
    std::string created_at;
    std::string dispatched_at;
    std::string completed_at;
};

/// Durable relay arming: when the requester (working in home_thread) DMs a
/// peer, the peer's reply in the pair thread is mirrored into home_thread
/// and the requester is woken there. One-shot, TTL'd. Armed from ANY
/// thread kind (Phase 66: the 21:00 routine check-in blind spot).
struct RelayLinkRow {
    std::string pair_thread_id;
    std::string requester_id;
    std::string home_thread_id;
    std::string created_at;
    bool        consumed = false;
};

class DeliveryStore {
public:
    DeliveryStore();                                   // default XDG path
    explicit DeliveryStore(std::filesystem::path db);  // tests / custom
    ~DeliveryStore();

    // ── deliveries ────────────────────────────────────────────────────
    /// Insert a new delivery (state=queued). Generates the id when empty.
    /// Returns the id.
    std::string enqueue(Delivery d);
    std::optional<Delivery> get(const std::string& id);
    /// Oldest queued delivery for an agent (FIFO drain order).
    std::optional<Delivery> oldest_queued(const std::string& target_agent_id);
    /// Idempotency: true when a queued/dispatched wake for this
    /// (target, source message) already exists — suppresses dup wakes.
    bool has_live_wake(const std::string& target_agent_id,
                       const std::string& source_message_id);
    void mark_dispatched(const std::string& id, const std::string& task_id);
    /// Reverse lookup by dispatched task id (turn completion paths).
    std::optional<Delivery> find_by_task(const std::string& task_id);
    /// ALL rows sharing a dispatched task id. When try_dispatch JOINS
    /// queued generation-0 wakes into one prompt (Plan.md, 2026-10-04),
    /// every joined row carries the same task_id; completion/watchdog/
    /// cancel paths must transition all of them (FIFO rowid order).
    std::vector<Delivery> find_all_by_task(const std::string& task_id);
    /// Queued wakes for one agent+thread at ONE fan-in depth, FIFO
    /// order — the JOIN set try_dispatch collapses into a single
    /// prompt. Generation-scoped: mixing depths in one turn would
    /// break complete_turn's fan-in continuation (gen+1). Gen-0 user
    /// bursts and gen>0 agent bursts both join (Plan.md, 2026-10-04);
    /// the circuit breaker counts dispatches (turns), and a joined
    /// batch is still one turn, so its trip semantics are unchanged.
    std::vector<Delivery> queued_for_thread(
        const std::string& target_agent_id, const std::string& thread_id,
        int generation, int limit);
    void mark_done(const std::string& id);
    void mark_error(const std::string& id, const std::string& error);
    void mark_dropped(const std::string& id, const std::string& reason);
    /// Boot recovery: rows left 'dispatched' by a dead process go back to
    /// 'queued'. Returns the number requeued.
    int  requeue_orphans();
    /// Wake TTL: queued rows older than ttl_s become 'expired'
    /// (the message stays in the thread; only the wake dies). Count out.
    int  expire_queued(int ttl_s);
    /// Status surface: filter by agent (sender OR target) and/or state,
    /// newest first. Empty filter = match all.
    std::vector<Delivery> list(const std::string& agent_id = "",
                               const std::string& state = "",
                               int limit = 50);
    /// An agent's own recent sends (sender_agent_id match), newest first.
    std::vector<Delivery> sent_by(const std::string& sender_agent_id,
                                  int limit = 10);
    /// Housekeeping: delete terminal rows older than `days`. Returns count.
    int prune_terminal_older_than(int days);

    // ── relay links ───────────────────────────────────────────────────
    /// Arm (or re-arm: latest wins) a relay for a pair thread.
    void arm_relay(const std::string& pair_thread_id,
                   const std::string& requester_id,
                   const std::string& home_thread_id);
    /// Fetch-and-consume the armed link for a pair thread, only when the
    /// replier is NOT the requester. Returns nullopt when none armed or
    /// the replier IS the requester (requester's own msgs never relay).
    std::optional<RelayLinkRow> consume_relay(
        const std::string& pair_thread_id, const std::string& replier_id);
    /// Drop links older than ttl_s. Returns count.
    int expire_relays(int ttl_s);

    // ── thread read high-water marks ──────────────────────────────────
    /// Advance an agent's read mark in a thread. Monotonic: a mark never
    /// moves backwards (message_created_at must exceed the stored mark).
    void mark_read(const std::string& thread_id, const std::string& agent_id,
                   const std::string& message_id,
                   const std::string& message_created_at);
    /// The agent's read mark (message created_at HWM), nullopt = never read.
    std::optional<std::string> read_mark(const std::string& thread_id,
                                         const std::string& agent_id);
    /// Unread count: messages in the thread newer than the agent's mark.
    /// Intended to run against ThreadStore's messages table — pass the
    /// count computed there. (Implemented in ThreadStore; declared here
    /// for API completeness in Phase 66c.)

private:
    std::filesystem::path db_path_;
    void init_schema();
};

}  // namespace hivemind
