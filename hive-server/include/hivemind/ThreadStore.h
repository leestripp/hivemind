#pragma once

#include <optional>
#include <string>
#include <unordered_map>
#include <vector>
#include <filesystem>
#include <sqlite3.h>

/**
 * ThreadStore — SQLite persistence for chat threads (HiveMind 2.0, Phase 1).
 *
 * A Thread is a conversation between the user and one or more roster agents
 * (kind "dm" = one agent, "group" = many). Participants are roster agent ids;
 * the human user is implicit (sender_kind "user" on messages).
 *
 * DB file: ~/.local/share/hivemind/threads.db (created on first access).
 * Thread-safety: fresh connection per op (same pattern as GraphStore).
 */
/// Phase 44: the singleton, non-deletable User Inbox thread. Fixed id so
/// the hub, worker relay and client all agree without a lookup. Kind
/// "user", zero agent participants — agents post notifications to the
/// human here via ChatService::post_user_inbox_message; because it has no
/// participants, nothing posted can fan in and start an agent turn.
inline constexpr const char* kUserThreadId = "user";

struct Thread {
    std::string id;
    std::string title;
    std::string kind;        // "dm" | "group"
    std::string created_at;
    std::string last_activity;         // MAX(messages.created_at); "" if no messages
    std::vector<std::string> agent_ids;  // participants (loaded separately)
};

struct ThreadMessage {
    std::string id;
    std::string thread_id;
    std::string sender_kind; // "user" | "agent" | "system"
    std::string sender_id;   // agent id, "" for user/system
    std::string body;
    // Phase 18: JSON array of {"name","path","size"} — files the user
    // attached; they live under <workspace>/attachments/<thread_id>/.
    // "[]" when the message has none.
    std::string attachments_json;
    // Phase 64: per-turn agent stats (tokens, prompt-cache hits,
    // iterations, context size) as a JSON object; "" when absent.
    std::string stats_json;
    std::string created_at;
};

class ThreadStore {
public:
    ThreadStore();                                  // default XDG path
    explicit ThreadStore(std::filesystem::path db); // tests / custom path
    ~ThreadStore();

    /// Create a thread with the given agent participants. Returns the Thread.
    /// Existing 1:1 dm thread with this agent, if any (Phase 11 dedup:
    /// clicking an agent in the roster must reopen the SAME conversation,
    /// not mint a fresh empty DM every time).
    /// Change a thread's kind (routine-thread migration 2026-09-23).
    void set_kind(const std::string& id, const std::string& kind);

    std::optional<Thread> find_dm(const std::string& agent_id);

    /// Idempotent: create the User Inbox thread if it does not exist
    /// (called on hub boot). Returns the thread.
    Thread ensure_user_thread();
    static bool is_user_thread(const std::string& id) { return id == kUserThreadId; }

    Thread create_thread(const std::string& title,
                         const std::string& kind,
                         const std::vector<std::string>& agent_ids);

    std::optional<Thread> get_thread(const std::string& id);
    std::vector<Thread> list_threads();
    /// Cascades messages + participants. The User Inbox (kUserThreadId)
    /// is protected: delete_thread returns false for it.
    bool delete_thread(const std::string& id);

    /// Phase 56: wipe a thread's messages, keep the thread. The User
    /// Inbox MAY be cleared (unlike delete) — it is the agent->human
    /// channel and only its history goes. Returns rows removed.
    int clear_messages(const std::string& thread_id);
    bool remove_participant(const std::string& thread_id,
                            const std::string& agent_id);
    bool add_participant(const std::string& thread_id,
                         const std::string& agent_id);

    /// Threads whose LATEST message is from the user and older than
    /// `older_than_iso`, with at least one agent participant — i.e. the
    /// agent never answered (dropped/failed turn). Agent/system replies
    /// and fresh messages exclude the thread (proactive nudges).
    std::vector<Thread> threads_awaiting_agent(const std::string& older_than_iso);

    /// Fetch one message by id (read-receipt HWM bookkeeping, Phase 66).
    std::optional<ThreadMessage> get_message(const std::string& id);

    /// Append a message; returns its id.
    std::string add_message(const std::string& thread_id,
                            const std::string& sender_kind,
                            const std::string& sender_id,
                            const std::string& body,
                            const std::string& attachments_json = "[]",
                            const std::string& stats_json = "");

    /// Messages in chronological order. If after_id is non-empty, only
    /// messages created after that message (exclusive) are returned.
    /// Per-sender message counts since an ISO-8601 UTC cutoff (System page
    /// usage stats). Key: sender_id (agent id / "user"), value: {agent,user}
    /// counts split by sender_kind.
    struct SenderCounts { int64_t agent = 0; int64_t user = 0; };
    std::unordered_map<std::string, SenderCounts>
    message_counts_since(const std::string& since_iso);

    /// Phase 55: bounded initial/page-back reads. No `before` → the
    /// LATEST `limit` messages; with `before` → up to `limit` messages
    /// older than it. Always chronological (ASC) on return. has_more
    /// is set when older messages remain beyond the window.
    std::vector<ThreadMessage> list_messages_window(
        const std::string& thread_id, const std::string& before_id,
        int limit, bool& has_more);
    std::vector<ThreadMessage> list_messages(const std::string& thread_id,
                                             const std::string& after_id = "");
    /// Phase 66c: messages newer than an ISO mark (""→ all) NOT sent by
    /// the human user — the unread-count basis for the user's badges.
    int count_non_user_since(const std::string& thread_id,
                             const std::string& since_iso);

private:
    std::filesystem::path db_path_;
    void ensure_schema();
    sqlite3* open_conn();
    std::vector<std::string> participants_of(sqlite3* db, const std::string& thread_id);
};
