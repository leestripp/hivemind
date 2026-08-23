#pragma once

#include <optional>
#include <string>
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
struct Thread {
    std::string id;
    std::string title;
    std::string kind;        // "dm" | "group"
    std::string created_at;
    std::vector<std::string> agent_ids;  // participants (loaded separately)
};

struct ThreadMessage {
    std::string id;
    std::string thread_id;
    std::string sender_kind; // "user" | "agent" | "system"
    std::string sender_id;   // agent id, "" for user/system
    std::string body;
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
    std::optional<Thread> find_dm(const std::string& agent_id);

    Thread create_thread(const std::string& title,
                         const std::string& kind,
                         const std::vector<std::string>& agent_ids);

    std::optional<Thread> get_thread(const std::string& id);
    std::vector<Thread> list_threads();
    bool delete_thread(const std::string& id);  // cascades messages + participants

    /// Append a message; returns its id.
    std::string add_message(const std::string& thread_id,
                            const std::string& sender_kind,
                            const std::string& sender_id,
                            const std::string& body);

    /// Messages in chronological order. If after_id is non-empty, only
    /// messages created after that message (exclusive) are returned.
    std::vector<ThreadMessage> list_messages(const std::string& thread_id,
                                             const std::string& after_id = "");

private:
    std::filesystem::path db_path_;
    void ensure_schema();
    sqlite3* open_conn();
    std::vector<std::string> participants_of(sqlite3* db, const std::string& thread_id);
};
