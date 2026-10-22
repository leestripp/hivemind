// =============================================================================
// src/ThreadStore.cpp — chat thread persistence (HiveMind 2.0, Plan.md Phase 1)
// =============================================================================
#include "hivemind/DbUtil.h"
#include "hivemind/ThreadStore.h"
#include "hivemind/Utf8.h"

#include <array>
#include <chrono>
#include <cstdio>
#include <stdexcept>
#include <string_view>

namespace fs = std::filesystem;

namespace {
using hivemind::db::uuid_v4;
inline std::string iso_now() { return hivemind::db::iso_now_us(); }
using hivemind::db::bind_text;
using hivemind::db::col_text;
inline void exec_sql(sqlite3* db, const char* sql) {
    hivemind::db::exec_sql(db, sql, "ThreadStore");
}




// ISO timestamp with a monotonic microsecond suffix so messages posted in
// the same second still sort in insertion order.








ThreadMessage row_to_message(sqlite3_stmt* st) {
    ThreadMessage m;
    m.id               = col_text(st, 0);
    m.thread_id        = col_text(st, 1);
    m.sender_kind      = col_text(st, 2);
    m.sender_id        = col_text(st, 3);
    m.body             = col_text(st, 4);
    m.attachments_json = col_text(st, 5);
    m.stats_json       = col_text(st, 7);
    m.created_at       = col_text(st, 6);
    // §MsgStatus: trailing columns 8-11 ("" when unset).
    m.status           = col_text(st, 8);
    m.status_by        = col_text(st, 9);
    m.status_at        = col_text(st, 10);
    m.status_note      = col_text(st, 11);
    return m;
}

} // namespace

ThreadStore::ThreadStore() {
    db_path_ = fs::path{std::getenv("HOME") ? std::getenv("HOME") : "."}
             / ".local" / "share" / "hivemind" / "threads.db";
    fs::create_directories(db_path_.parent_path());
    ensure_schema();
}

ThreadStore::ThreadStore(fs::path db) : db_path_(std::move(db)) {
    fs::create_directories(db_path_.parent_path());
    ensure_schema();
}

ThreadStore::~ThreadStore() = default;

sqlite3* ThreadStore::open_conn() {
    sqlite3* db = nullptr;
    if (sqlite3_open(db_path_.string().c_str(), &db) != SQLITE_OK) {
        std::string msg = db ? sqlite3_errmsg(db) : "out of memory";
        if (db) sqlite3_close(db);
        throw std::runtime_error("ThreadStore open failed: " + msg);
    }
    // Wait up to 5s on a contended write lock instead of
    // failing instantly — concurrent POSTs (phone notification
    // catch-up bursts) otherwise 500 with "database is locked".
    sqlite3_busy_timeout(db, 5000);
    return db;
}

void ThreadStore::ensure_schema() {
    sqlite3* db = open_conn();
    exec_sql(db,
        "CREATE TABLE IF NOT EXISTS threads ("
        "  id TEXT PRIMARY KEY,"
        "  title TEXT NOT NULL DEFAULT '',"
        "  kind TEXT NOT NULL DEFAULT 'dm',"
        "  created_at TEXT NOT NULL"
        ")");
    exec_sql(db,
        "CREATE TABLE IF NOT EXISTS thread_participants ("
        "  thread_id TEXT NOT NULL,"
        "  agent_id TEXT NOT NULL,"
        "  PRIMARY KEY (thread_id, agent_id)"
        ")");
    exec_sql(db,
        "CREATE TABLE IF NOT EXISTS messages ("
        "  id TEXT PRIMARY KEY,"
        "  thread_id TEXT NOT NULL,"
        "  sender_kind TEXT NOT NULL,"
        "  sender_id TEXT NOT NULL DEFAULT '',"
        "  body TEXT NOT NULL DEFAULT '',"
        "  created_at TEXT NOT NULL"
        ")");
    exec_sql(db,
        "CREATE INDEX IF NOT EXISTS idx_messages_thread"
        " ON messages(thread_id, created_at)");
    // Phase 67: user thread groups (folders — members PK enforces one
    // group per thread; smart groups are computed, never stored).
    exec_sql(db,
        "CREATE TABLE IF NOT EXISTS thread_groups ("
        "  id TEXT PRIMARY KEY,"
        "  name TEXT NOT NULL,"
        "  position INTEGER NOT NULL DEFAULT 0,"
        "  created_at TEXT NOT NULL"
        ")");
    exec_sql(db,
        "CREATE TABLE IF NOT EXISTS thread_group_members ("
        "  thread_id TEXT PRIMARY KEY,"
        "  group_id TEXT NOT NULL"
        ")");
    // Phase 18 migration: attachments column on pre-existing DBs.
    bool has_att = false;
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db, "PRAGMA table_info(messages)",
                           -1, &st, nullptr) == SQLITE_OK) {
        while (sqlite3_step(st) == SQLITE_ROW) {
            const char* n = col_text(st, 1);
            if (n && std::string_view{n} == "attachments_json") {
                has_att = true; break;
            }
        }
        sqlite3_finalize(st);
    }
    if (!has_att)
        exec_sql(db,
            "ALTER TABLE messages ADD COLUMN attachments_json"
            " TEXT NOT NULL DEFAULT '[]'");
    // Phase 64 migration: per-turn stats column (same pattern).
    bool has_stats = false;
    if (sqlite3_prepare_v2(db, "PRAGMA table_info(messages)",
                           -1, &st, nullptr) == SQLITE_OK) {
        while (sqlite3_step(st) == SQLITE_ROW) {
            const char* n = col_text(st, 1);
            if (n && std::string_view{n} == "stats_json") {
                has_stats = true; break;
            }
        }
        sqlite3_finalize(st);
    }
    if (!has_stats)
        exec_sql(db,
            "ALTER TABLE messages ADD COLUMN stats_json"
            " TEXT NOT NULL DEFAULT ''");

    // §MsgStatus (2026-10-22): task-lifecycle columns. Same ADD-if-
    // missing pattern; one PRAGMA scan for all four.
    {
        bool has_status = false;
        sqlite3_stmt* st2 = nullptr;
        if (sqlite3_prepare_v2(db, "PRAGMA table_info(messages)",
                               -1, &st2, nullptr) == SQLITE_OK) {
            while (sqlite3_step(st2) == SQLITE_ROW) {
                const char* n = col_text(st2, 1);
                if (n && std::string_view{n} == "status") {
                    has_status = true; break;
                }
            }
            sqlite3_finalize(st2);
        }
        if (!has_status) {
            exec_sql(db, "ALTER TABLE messages ADD COLUMN status"
                         " TEXT NOT NULL DEFAULT ''");
            exec_sql(db, "ALTER TABLE messages ADD COLUMN status_by"
                         " TEXT NOT NULL DEFAULT ''");
            exec_sql(db, "ALTER TABLE messages ADD COLUMN status_at"
                         " TEXT NOT NULL DEFAULT ''");
            exec_sql(db, "ALTER TABLE messages ADD COLUMN status_note"
                         " TEXT NOT NULL DEFAULT ''");
        }
    }
    sqlite3_close(db);
}

std::vector<std::string> ThreadStore::participants_of(sqlite3* db,
                                                      const std::string& thread_id) {
    std::vector<std::string> out;
    sqlite3_stmt* st = nullptr;
    const char* sql =
        "SELECT agent_id FROM thread_participants WHERE thread_id = ?"
        " ORDER BY rowid ASC";
    if (sqlite3_prepare_v2(db, sql, -1, &st, nullptr) == SQLITE_OK) {
        bind_text(st, 1, thread_id);
        while (sqlite3_step(st) == SQLITE_ROW) out.emplace_back(col_text(st, 0));
        sqlite3_finalize(st);
    }
    return out;
}

void ThreadStore::set_kind(const std::string& id, const std::string& kind) {
    sqlite3* db = open_conn();
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db, "UPDATE threads SET kind=? WHERE id=?",
                           -1, &st, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, kind.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 2, id.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_step(st);
    }
    sqlite3_finalize(st);
    sqlite3_close(db);
}

std::optional<Thread> ThreadStore::find_dm(const std::string& agent_id) {
    // Canonical DM = the OLDEST 1:1 thread for the agent. list_threads()
    // is ordered by last activity DESC, so returning the first match would
    // hand back the most recently ACTIVE dm thread — any newer 1:1 thread
    // (a client's "New thread" click, a bare API-created single-agent
    // thread) would shadow the real DM and hijack every client that
    // resolves "the DM with X" (Android voice app, roster clicks).
    // 2026-09-23 (routine threads) + 2026-09-30 (check-in threads)
    // incidents. Resolve deterministically to the oldest: later threads
    // may coexist, but they can never hijack the canonical DM.
    std::optional<Thread> canonical;
    for (auto& t : list_threads()) {
        if (t.kind != "dm" || t.agent_ids.size() != 1) continue;
        if (t.agent_ids[0] != agent_id) continue;
        if (!canonical || t.created_at < canonical->created_at)
            canonical = t;
    }
    return canonical;
}

Thread ThreadStore::ensure_user_thread() {
    if (auto t = get_thread(kUserThreadId)) return *t;
    Thread t;
    t.id = kUserThreadId;
    t.title = "You (inbox)";
    t.kind = "user";
    t.created_at = iso_now();
    sqlite3* db = open_conn();
    sqlite3_stmt* st = nullptr;
    // INSERT OR IGNORE: two hubs racing boot (or a test re-open) must not throw.
    if (sqlite3_prepare_v2(db,
            "INSERT OR IGNORE INTO threads (id, title, kind, created_at)"
            " VALUES (?,?,?,?)", -1, &st, nullptr) == SQLITE_OK) {
        bind_text(st, 1, t.id);
        bind_text(st, 2, t.title);
        bind_text(st, 3, t.kind);
        bind_text(st, 4, t.created_at);
        sqlite3_step(st);
        sqlite3_finalize(st);
    }
    sqlite3_close(db);
    return t;
}

Thread ThreadStore::create_thread(const std::string& title,
                                  const std::string& kind,
                                  const std::vector<std::string>& agent_ids) {
    Thread t;
    t.id = uuid_v4();
    t.title = title;
    t.kind = (kind == "user")                    ? "user"
           : (kind == "group" || agent_ids.size() > 1) ? "group" : "dm";
    t.created_at = iso_now();
    t.agent_ids = agent_ids;

    sqlite3* db = open_conn();
    exec_sql(db, "BEGIN");
    try {
        sqlite3_stmt* st = nullptr;
        if (sqlite3_prepare_v2(db,
                "INSERT INTO threads (id, title, kind, created_at) VALUES (?,?,?,?)",
                -1, &st, nullptr) != SQLITE_OK)
            throw std::runtime_error(sqlite3_errmsg(db));
        bind_text(st, 1, t.id);
        bind_text(st, 2, t.title);
        bind_text(st, 3, t.kind);
        bind_text(st, 4, t.created_at);
        if (sqlite3_step(st) != SQLITE_DONE)
            throw std::runtime_error("thread insert failed");
        sqlite3_finalize(st);

        for (const auto& aid : agent_ids) {
            sqlite3_stmt* ps = nullptr;
            if (sqlite3_prepare_v2(db,
                    "INSERT INTO thread_participants (thread_id, agent_id)"
                    " VALUES (?,?)", -1, &ps, nullptr) != SQLITE_OK)
                throw std::runtime_error(sqlite3_errmsg(db));
            bind_text(ps, 1, t.id);
            bind_text(ps, 2, aid);
            if (sqlite3_step(ps) != SQLITE_DONE)
                throw std::runtime_error("participant insert failed");
            sqlite3_finalize(ps);
        }
        exec_sql(db, "COMMIT");
    } catch (...) {
        exec_sql(db, "ROLLBACK");
        sqlite3_close(db);
        throw;
    }
    sqlite3_close(db);
    return t;
}

std::optional<Thread> ThreadStore::get_thread(const std::string& id) {
    sqlite3* db = open_conn();
    std::optional<Thread> out;
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db,
            "SELECT id, title, kind, created_at FROM threads WHERE id = ?",
            -1, &st, nullptr) == SQLITE_OK) {
        bind_text(st, 1, id);
        if (sqlite3_step(st) == SQLITE_ROW) {
            Thread t;
            t.id = col_text(st, 0);
            t.title = col_text(st, 1);
            t.kind = col_text(st, 2);
            t.created_at = col_text(st, 3);
            out = std::move(t);
        }
        sqlite3_finalize(st);
    }
    if (out) out->agent_ids = participants_of(db, id);
    sqlite3_close(db);
    return out;
}

std::set<std::string> ThreadStore::user_posted_thread_ids() {
    sqlite3* db = open_conn();
    sqlite3_stmt* st = nullptr;
    std::set<std::string> out;
    if (sqlite3_prepare_v2(db,
            "SELECT DISTINCT thread_id FROM messages WHERE sender_kind = 'user'",
            -1, &st, nullptr) == SQLITE_OK) {
        while (sqlite3_step(st) == SQLITE_ROW) out.insert(col_text(st, 0));
    }
    sqlite3_finalize(st);
    sqlite3_close(db);
    return out;
}

int ThreadStore::count_non_user_since(const std::string& thread_id,
                                      const std::string& since_iso) {
    sqlite3* db = open_conn();
    sqlite3_stmt* st = nullptr;
    int n = 0;
    if (sqlite3_prepare_v2(db,
            "SELECT COUNT(*) FROM messages"
            " WHERE thread_id = ? AND sender_kind != 'user'"
            " AND created_at > ?",
            -1, &st, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, thread_id.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 2, since_iso.c_str(), -1, SQLITE_TRANSIENT);
        if (sqlite3_step(st) == SQLITE_ROW) n = sqlite3_column_int(st, 0);
    }
    sqlite3_finalize(st);
    sqlite3_close(db);   // per-op connection — a missing close leaks an
                         // fd per call; at 30 threads per /api/threads
                         // poll this exhausted the hub's 1024-fd limit
                         // overnight (EMFILE → AuthStore 500s). 2026-09-28.
    return n;
}

std::vector<Thread> ThreadStore::list_threads() {
    sqlite3* db = open_conn();
    std::vector<Thread> out;
    sqlite3_stmt* st = nullptr;
    // last_activity lets clients show unread badges without polling every
    // thread's messages (Phase 14.1).
    if (sqlite3_prepare_v2(db,
            "SELECT t.id, t.title, t.kind, t.created_at,"
            "  (SELECT MAX(m.created_at) FROM messages m WHERE m.thread_id = t.id)"
            " FROM threads t"
            " ORDER BY COALESCE((SELECT MAX(m.created_at) FROM messages m"
            "                   WHERE m.thread_id = t.id), t.created_at) DESC",
            -1, &st, nullptr) == SQLITE_OK) {
        while (sqlite3_step(st) == SQLITE_ROW) {
            Thread t;
            t.id = col_text(st, 0);
            t.title = col_text(st, 1);
            t.kind = col_text(st, 2);
            t.created_at = col_text(st, 3);
            t.last_activity = col_text(st, 4);
            out.push_back(std::move(t));
        }
        sqlite3_finalize(st);
    }
    for (auto& t : out) t.agent_ids = participants_of(db, t.id);
    sqlite3_close(db);
    return out;
}

std::vector<Thread> ThreadStore::threads_awaiting_agent(
    const std::string& older_than_iso) {
    sqlite3* db = open_conn();
    std::vector<Thread> out;
    sqlite3_stmt* st = nullptr;
    // Latest message per thread (created_at + rowid tiebreak) must be a
    // user message older than the threshold; thread needs a participant.
    // The nudge itself is posted as sender_kind 'system', so a nudged
    // thread drops out of this set until the next user message — one
    // nudge per unanswered message, no state table needed.
    const char* sql =
        "SELECT t.id, t.title, t.kind, t.created_at"
        " FROM threads t"
        " WHERE EXISTS (SELECT 1 FROM thread_participants p"
        "              WHERE p.thread_id = t.id)"
        "   AND (SELECT m.sender_kind FROM messages m"
        "        WHERE m.thread_id = t.id"
        "        ORDER BY m.created_at DESC, m.rowid DESC LIMIT 1) = 'user'"
        "   AND (SELECT m.created_at FROM messages m"
        "        WHERE m.thread_id = t.id"
        "        ORDER BY m.created_at DESC, m.rowid DESC LIMIT 1) < ?";
    if (sqlite3_prepare_v2(db, sql, -1, &st, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, older_than_iso.c_str(), -1, SQLITE_TRANSIENT);
        while (sqlite3_step(st) == SQLITE_ROW) {
            Thread t;
            t.id = col_text(st, 0);
            t.title = col_text(st, 1);
            t.kind = col_text(st, 2);
            t.created_at = col_text(st, 3);
            out.push_back(std::move(t));
        }
        sqlite3_finalize(st);
    }
    for (auto& t : out) t.agent_ids = participants_of(db, t.id);
    sqlite3_close(db);
    return out;
}

bool ThreadStore::add_participant(const std::string& thread_id,
                                  const std::string& agent_id) {
    sqlite3* db = open_conn();
    sqlite3_stmt* st = nullptr;
    const char* sql =
        "INSERT OR IGNORE INTO thread_participants (thread_id, agent_id)"
        " VALUES (?, ?)";
    if (sqlite3_prepare_v2(db, sql, -1, &st, nullptr) != SQLITE_OK) {
        sqlite3_close(db);
        return false;
    }
    bind_text(st, 1, thread_id);
    bind_text(st, 2, agent_id);
    bool ok = sqlite3_step(st) == SQLITE_DONE;
    sqlite3_finalize(st);
    sqlite3_close(db);
    return ok;
}

bool ThreadStore::remove_participant(const std::string& thread_id,
                                     const std::string& agent_id) {
    sqlite3* db = open_conn();
    sqlite3_stmt* st = nullptr;
    const char* sql =
        "DELETE FROM thread_participants WHERE thread_id = ? AND agent_id = ?";
    if (sqlite3_prepare_v2(db, sql, -1, &st, nullptr) != SQLITE_OK) {
        sqlite3_close(db);
        return false;
    }
    bind_text(st, 1, thread_id);
    bind_text(st, 2, agent_id);
    bool ok = sqlite3_step(st) == SQLITE_DONE && sqlite3_changes(db) > 0;
    sqlite3_finalize(st);
    sqlite3_close(db);
    return ok;
}

int ThreadStore::clear_messages(const std::string& thread_id) {
    sqlite3* db = open_conn();
    int removed = 0;
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db,
            "DELETE FROM messages WHERE thread_id = ?",
            -1, &st, nullptr) == SQLITE_OK) {
        bind_text(st, 1, thread_id);
        if (sqlite3_step(st) == SQLITE_DONE)
            removed = sqlite3_changes(db);
        sqlite3_finalize(st);
    }
    sqlite3_close(db);
    return removed;
}

bool ThreadStore::delete_thread(const std::string& id) {
    if (is_user_thread(id)) return false;   // Phase 44: inbox is protected
    sqlite3* db = open_conn();
    exec_sql(db, "BEGIN");
    bool ok = false;
    try {
        sqlite3_stmt* st = nullptr;
        auto del = [&](const char* sql) {
            if (sqlite3_prepare_v2(db, sql, -1, &st, nullptr) != SQLITE_OK)
                throw std::runtime_error(sqlite3_errmsg(db));
            bind_text(st, 1, id);
            bool done = sqlite3_step(st) == SQLITE_DONE;
            int changes = sqlite3_changes(db);
            sqlite3_finalize(st);
            return done && changes > 0;
        };
        del("DELETE FROM messages WHERE thread_id = ?");
        del("DELETE FROM thread_participants WHERE thread_id = ?");
        ok = del("DELETE FROM threads WHERE id = ?");
        exec_sql(db, "COMMIT");
    } catch (...) {
        exec_sql(db, "ROLLBACK");
        sqlite3_close(db);
        throw;
    }
    sqlite3_close(db);
    return ok;
}

std::optional<ThreadMessage> ThreadStore::get_message(const std::string& id) {
    sqlite3* db = open_conn();
    std::optional<ThreadMessage> out;
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db,
            "SELECT id, thread_id, sender_kind, sender_id, body,"
            " attachments_json, created_at, stats_json,"
          " status, status_by, status_at, status_note"
            " FROM messages WHERE id = ?",
            -1, &st, nullptr) == SQLITE_OK) {
        bind_text(st, 1, id);
        if (sqlite3_step(st) == SQLITE_ROW) out = row_to_message(st);
        sqlite3_finalize(st);
    }
    sqlite3_close(db);
    return out;
}

void ThreadStore::set_message_status(const std::string& thread_id,
                                     const std::string& message_id,
                                     const std::string& status,
                                     const std::string& by,
                                     const std::string& note) {
    sqlite3* db = open_conn();
    sqlite3_stmt* st = nullptr;
    // Empty status clears all four fields; otherwise stamp server-side.
    const std::string at = status.empty() ? "" : iso_now();
    const char* sql =
        "UPDATE messages SET status = ?, status_by = ?, status_at = ?,"
        " status_note = ? WHERE id = ? AND thread_id = ?";
    if (sqlite3_prepare_v2(db, sql, -1, &st, nullptr) != SQLITE_OK) {
        std::string msg = sqlite3_errmsg(db);
        sqlite3_close(db);
        throw std::runtime_error("ThreadStore status prepare failed: " + msg);
    }
    bind_text(st, 1, status);
    bind_text(st, 2, status.empty() ? "" : by);
    bind_text(st, 3, at);
    bind_text(st, 4, status.empty() ? "" : note);
    bind_text(st, 5, message_id);
    bind_text(st, 6, thread_id);
    const bool ok = sqlite3_step(st) == SQLITE_DONE;
    const int changes = sqlite3_changes(db);
    sqlite3_finalize(st);
    sqlite3_close(db);
    if (!ok || changes == 0)
        throw std::invalid_argument("message not found");
}

std::string ThreadStore::add_message(const std::string& thread_id,
                                     const std::string& sender_kind,
                                     const std::string& sender_id,
                                     const std::string& body,
                                     const std::string& attachments_json,
                                     const std::string& stats_json) {
    std::string id = uuid_v4();
    std::string now = iso_now();
    sqlite3* db = open_conn();
    sqlite3_stmt* st = nullptr;
    const char* sql =
        "INSERT INTO messages (id, thread_id, sender_kind, sender_id, body,"
        "                     attachments_json, created_at, stats_json)"
        " VALUES (?,?,?,?,?,?,?,?)";
    if (sqlite3_prepare_v2(db, sql, -1, &st, nullptr) != SQLITE_OK) {
        std::string msg = sqlite3_errmsg(db);
        sqlite3_close(db);
        throw std::runtime_error("ThreadStore message prepare failed: " + msg);
    }
    bind_text(st, 1, id);
    bind_text(st, 2, thread_id);
    bind_text(st, 3, sender_kind);
    bind_text(st, 4, sender_id);
    // 2026-10-22 (bug sweep B6): repair untrusted text at the store
    // ingress — one invalid UTF-8 byte here poisons every later
    // nlohmann dump of the thread (2026-10-02 incident).
    bind_text(st, 5, hivemind::sanitize_utf8(body));
    bind_text(st, 6, attachments_json.empty() ? "[]" : attachments_json);
    bind_text(st, 7, now);
    bind_text(st, 8, stats_json);
    bool ok = sqlite3_step(st) == SQLITE_DONE;
    sqlite3_finalize(st);
    sqlite3_close(db);
    if (!ok) throw std::runtime_error("ThreadStore message insert failed");
    return id;
}

std::vector<ThreadMessage> ThreadStore::list_messages_window(
        const std::string& thread_id, const std::string& before_id,
        int limit, bool& has_more) {
    has_more = false;
    sqlite3* db = open_conn();
    std::vector<ThreadMessage> out;

    std::string before_ts;
    if (!before_id.empty()) {
        sqlite3_stmt* st = nullptr;
        if (sqlite3_prepare_v2(db,
                "SELECT created_at FROM messages WHERE id = ?",
                -1, &st, nullptr) == SQLITE_OK) {
            bind_text(st, 1, before_id);
            if (sqlite3_step(st) == SQLITE_ROW) before_ts = col_text(st, 0);
            sqlite3_finalize(st);
        }
    }

    // Newest-first tail of (everything | everything older than before),
    // fetching limit+1 so a surplus row flags has_more; reversed to ASC
    // for the caller.
    const char* sql = before_ts.empty()
        ? "SELECT id, thread_id, sender_kind, sender_id, body,"
          " attachments_json, created_at, stats_json,"
          " status, status_by, status_at, status_note"
          " FROM messages WHERE thread_id = ?"
          " ORDER BY created_at DESC, rowid DESC LIMIT ?"
        : "SELECT id, thread_id, sender_kind, sender_id, body,"
          " attachments_json, created_at, stats_json,"
          " status, status_by, status_at, status_note"
          " FROM messages WHERE thread_id = ? AND created_at < ?"
          " ORDER BY created_at DESC, rowid DESC LIMIT ?";
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db, sql, -1, &st, nullptr) == SQLITE_OK) {
        bind_text(st, 1, thread_id);
        if (!before_ts.empty()) bind_text(st, 2, before_ts);
        sqlite3_bind_int(st, before_ts.empty() ? 2 : 3, limit + 1);
        while (sqlite3_step(st) == SQLITE_ROW)
            out.push_back(row_to_message(st));
        sqlite3_finalize(st);
    }
    sqlite3_close(db);

    if (static_cast<int>(out.size()) > limit) {
        has_more = true;
        out.resize(limit);
    }
    std::reverse(out.begin(), out.end());
    return out;
}

std::vector<ThreadMessage> ThreadStore::list_messages(const std::string& thread_id,
                                                      const std::string& after_id) {
    sqlite3* db = open_conn();
    std::vector<ThreadMessage> out;

    std::string after_ts;
    if (!after_id.empty()) {
        sqlite3_stmt* st = nullptr;
        if (sqlite3_prepare_v2(db,
                "SELECT created_at FROM messages WHERE id = ?",
                -1, &st, nullptr) == SQLITE_OK) {
            bind_text(st, 1, after_id);
            if (sqlite3_step(st) == SQLITE_ROW) after_ts = col_text(st, 0);
            sqlite3_finalize(st);
        }
    }

    sqlite3_stmt* st = nullptr;
    const char* sql = after_ts.empty()
        ? "SELECT id, thread_id, sender_kind, sender_id, body,"
          " attachments_json, created_at, stats_json,"
          " status, status_by, status_at, status_note"
          " FROM messages WHERE thread_id = ? ORDER BY created_at ASC"
        : "SELECT id, thread_id, sender_kind, sender_id, body,"
          " attachments_json, created_at, stats_json,"
          " status, status_by, status_at, status_note"
          " FROM messages WHERE thread_id = ? AND created_at > ?"
          " ORDER BY created_at ASC";
    if (sqlite3_prepare_v2(db, sql, -1, &st, nullptr) == SQLITE_OK) {
        bind_text(st, 1, thread_id);
        if (!after_ts.empty()) bind_text(st, 2, after_ts);
        while (sqlite3_step(st) == SQLITE_ROW) out.push_back(row_to_message(st));
        sqlite3_finalize(st);
    }
    sqlite3_close(db);
    return out;
}

std::unordered_map<std::string, ThreadStore::SenderCounts>
ThreadStore::message_counts_since(const std::string& since_iso) {
    sqlite3* db = open_conn();
    std::unordered_map<std::string, SenderCounts> out;
    sqlite3_stmt* st = nullptr;
    const char* sql =
        "SELECT sender_id, sender_kind, COUNT(*) FROM messages"
        " WHERE created_at >= ? AND sender_kind IN ('agent','user')"
        " GROUP BY sender_id, sender_kind";
    if (sqlite3_prepare_v2(db, sql, -1, &st, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, since_iso.c_str(), -1, SQLITE_TRANSIENT);
        while (sqlite3_step(st) == SQLITE_ROW) {
            const char* id   = reinterpret_cast<const char*>(sqlite3_column_text(st, 0));
            const char* kind = reinterpret_cast<const char*>(sqlite3_column_text(st, 1));
            auto& c = out[id ? id : ""];
            if (kind && std::string(kind) == "agent") c.agent = sqlite3_column_int64(st, 2);
            else                                      c.user  = sqlite3_column_int64(st, 2);
        }
        sqlite3_finalize(st);
    }
    sqlite3_close(db);
    return out;
}

// ---- Phase 67: user thread groups (folders) ----
// Every function below closes its per-op connection — an unclosed
// open_conn() here once exhausted the hub's fd limit (2026-09-28).

std::string ThreadStore::create_group(const std::string& name) {
    if (name.empty()) throw std::invalid_argument("group name required");
    const std::string id = uuid_v4();
    sqlite3* db = open_conn();
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db,
            "INSERT INTO thread_groups (id, name, position, created_at)"
            " VALUES (?, ?,"
            "  COALESCE((SELECT MAX(position)+1 FROM thread_groups), 0),"
            "  ?)",
            -1, &st, nullptr) == SQLITE_OK) {
        bind_text(st, 1, id);
        bind_text(st, 2, name);
        bind_text(st, 3, iso_now());
        sqlite3_step(st);
    }
    sqlite3_finalize(st);
    sqlite3_close(db);
    return id;
}

void ThreadStore::rename_group(const std::string& id,
                               const std::string& name) {
    if (name.empty()) throw std::invalid_argument("group name required");
    sqlite3* db = open_conn();
    sqlite3_stmt* st = nullptr;
    int changed = 0;
    if (sqlite3_prepare_v2(db, "UPDATE thread_groups SET name=? WHERE id=?",
                           -1, &st, nullptr) == SQLITE_OK) {
        bind_text(st, 1, name);
        bind_text(st, 2, id);
        sqlite3_step(st);
        changed = sqlite3_changes(db);
    }
    sqlite3_finalize(st);
    sqlite3_close(db);
    if (changed == 0) throw std::invalid_argument("unknown group: " + id);
}

void ThreadStore::delete_group(const std::string& id) {
    sqlite3* db = open_conn();
    sqlite3_stmt* st = nullptr;
    // Revert members to their smart groups explicitly — foreign-key
    // enforcement is off by default on a fresh per-op connection, so
    // ON DELETE CASCADE would not fire.
    if (sqlite3_prepare_v2(db,
            "DELETE FROM thread_group_members WHERE group_id=?",
            -1, &st, nullptr) == SQLITE_OK) {
        bind_text(st, 1, id);
        sqlite3_step(st);
        sqlite3_finalize(st);
        st = nullptr;
    }
    int changed = 0;
    if (sqlite3_prepare_v2(db, "DELETE FROM thread_groups WHERE id=?",
                           -1, &st, nullptr) == SQLITE_OK) {
        bind_text(st, 1, id);
        sqlite3_step(st);
        changed = sqlite3_changes(db);
    }
    sqlite3_finalize(st);
    sqlite3_close(db);
    if (changed == 0) throw std::invalid_argument("unknown group: " + id);
}

void ThreadStore::set_thread_group(const std::string& thread_id,
                                   const std::string& group_id) {
    sqlite3* db = open_conn();
    sqlite3_stmt* st = nullptr;
    bool thread_exists = false;
    if (sqlite3_prepare_v2(db, "SELECT 1 FROM threads WHERE id=?",
                           -1, &st, nullptr) == SQLITE_OK) {
        bind_text(st, 1, thread_id);
        thread_exists = (sqlite3_step(st) == SQLITE_ROW);
        sqlite3_finalize(st);
        st = nullptr;
    }
    if (!thread_exists) {
        sqlite3_close(db);
        throw std::invalid_argument("unknown thread: " + thread_id);
    }
    if (group_id.empty()) {
        // Back to the smart group: just drop the membership row.
        if (sqlite3_prepare_v2(db,
                "DELETE FROM thread_group_members WHERE thread_id=?",
                -1, &st, nullptr) == SQLITE_OK) {
            bind_text(st, 1, thread_id);
            sqlite3_step(st);
        }
        sqlite3_finalize(st);
        sqlite3_close(db);
        return;
    }
    bool group_exists = false;
    if (sqlite3_prepare_v2(db, "SELECT 1 FROM thread_groups WHERE id=?",
                           -1, &st, nullptr) == SQLITE_OK) {
        bind_text(st, 1, group_id);
        group_exists = (sqlite3_step(st) == SQLITE_ROW);
        sqlite3_finalize(st);
        st = nullptr;
    }
    if (!group_exists) {
        sqlite3_close(db);
        throw std::invalid_argument("unknown group: " + group_id);
    }
    // INSERT OR REPLACE = the folder rule (re-moving replaces, never
    // duplicates) via the thread_id PRIMARY KEY.
    if (sqlite3_prepare_v2(db,
            "INSERT OR REPLACE INTO thread_group_members"
            " (thread_id, group_id) VALUES (?, ?)",
            -1, &st, nullptr) == SQLITE_OK) {
        bind_text(st, 1, thread_id);
        bind_text(st, 2, group_id);
        sqlite3_step(st);
    }
    sqlite3_finalize(st);
    sqlite3_close(db);
}

std::vector<ThreadGroup> ThreadStore::list_groups() {
    std::vector<ThreadGroup> out;
    sqlite3* db = open_conn();
    sqlite3_stmt* st = nullptr;
    const char* sql =
        "SELECT g.id, g.name, g.position,"
        " (SELECT COUNT(*) FROM thread_group_members m"
        "  WHERE m.group_id = g.id)"
        " FROM thread_groups g ORDER BY g.position ASC, g.created_at ASC";
    if (sqlite3_prepare_v2(db, sql, -1, &st, nullptr) == SQLITE_OK) {
        while (sqlite3_step(st) == SQLITE_ROW) {
            ThreadGroup g;
            const char* v = col_text(st, 0); if (v) g.id = v;
            v = col_text(st, 1); if (v) g.name = v;
            g.position = sqlite3_column_int(st, 2);
            g.member_count = sqlite3_column_int(st, 3);
            out.push_back(std::move(g));
        }
        sqlite3_finalize(st);
    }
    sqlite3_close(db);
    return out;
}

std::unordered_map<std::string, std::string>
ThreadStore::thread_group_map() {
    std::unordered_map<std::string, std::string> out;
    sqlite3* db = open_conn();
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db,
            "SELECT thread_id, group_id FROM thread_group_members",
            -1, &st, nullptr) == SQLITE_OK) {
        while (sqlite3_step(st) == SQLITE_ROW) {
            const char* t = col_text(st, 0);
            const char* g = col_text(st, 1);
            if (t && g) out[t] = g;
        }
        sqlite3_finalize(st);
    }
    sqlite3_close(db);
    return out;
}
