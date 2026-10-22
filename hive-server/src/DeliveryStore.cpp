// =============================================================================
// DeliveryStore (Phase 66a): durable wake/relay queue + read high-water
// marks. See hivemind/DeliveryStore.h and Plan.md (Phase 66).
// =============================================================================

#include "hivemind/DeliveryStore.h"
#include "hivemind/Utf8.h"
#include "hivemind/DbUtil.h"

#include <sqlite3.h>

#include <cstdlib>
#include <stdexcept>

using hivemind::db::bind_text;
using hivemind::db::col_text;
using hivemind::db::exec_sql;
using hivemind::db::iso_now;
using hivemind::db::uuid_v4;

namespace fs = std::filesystem;

namespace {

sqlite3* open_db(const fs::path& p) {
    sqlite3* db = nullptr;
    if (sqlite3_open(p.string().c_str(), &db) != SQLITE_OK) {
        std::string msg = db ? sqlite3_errmsg(db) : "out of memory";
        if (db) sqlite3_close(db);
        throw std::runtime_error("DeliveryStore: cannot open " +
                                 p.string() + ": " + msg);
    }
    // Wait up to 5s on a contended write lock instead of
    // failing instantly (concurrent POST bursts otherwise
    // 500 with "database is locked").
    sqlite3_busy_timeout(db, 5000);
    return db;
}

struct DbGuard {
    sqlite3* db;
    ~DbGuard() { sqlite3_close(db); }
};

struct StmtGuard {
    sqlite3_stmt* st = nullptr;
    ~StmtGuard() { if (st) sqlite3_finalize(st); }
    operator sqlite3_stmt*() const { return st; }
};

void prepare(sqlite3* db, StmtGuard& st, const char* sql, const char* what) {
    if (sqlite3_prepare_v2(db, sql, -1, &st.st, nullptr) != SQLITE_OK)
        throw std::runtime_error(std::string("DeliveryStore: prepare ") +
                                 what + ": " + sqlite3_errmsg(db));
}

void step_done(sqlite3* db, sqlite3_stmt* st, const char* what) {
    if (sqlite3_step(st) != SQLITE_DONE)
        throw std::runtime_error(std::string("DeliveryStore: ") + what +
                                 ": " + sqlite3_errmsg(db));
}

constexpr const char* kCols =
    "id, kind, thread_id, target_agent_id, sender_agent_id, sender_label,"
    " source, body, generation, state, task_id, source_message_id, error,"
    " created_at, dispatched_at, completed_at";

hivemind::Delivery row_to_delivery(sqlite3_stmt* st) {
    hivemind::Delivery d;
    d.id                = col_text(st, 0);
    d.kind              = col_text(st, 1);
    d.thread_id         = col_text(st, 2);
    d.target_agent_id   = col_text(st, 3);
    d.sender_agent_id   = col_text(st, 4);
    d.sender_label      = col_text(st, 5);
    d.source            = col_text(st, 6);
    d.body              = col_text(st, 7);
    d.generation        = sqlite3_column_int(st, 8);
    d.state             = col_text(st, 9);
    d.task_id           = col_text(st, 10);
    d.source_message_id = col_text(st, 11);
    d.error             = col_text(st, 12);
    d.created_at        = col_text(st, 13);
    d.dispatched_at     = col_text(st, 14);
    d.completed_at      = col_text(st, 15);
    return d;
}

}  // namespace

namespace hivemind {

DeliveryStore::DeliveryStore() {
    db_path_ = fs::path{std::getenv("HOME") ? std::getenv("HOME") : "."}
               / ".local" / "share" / "hivemind" / "deliveries.db";
    fs::create_directories(db_path_.parent_path());
    init_schema();
}

DeliveryStore::DeliveryStore(fs::path db) : db_path_(std::move(db)) {
    fs::create_directories(db_path_.parent_path());
    init_schema();
}

DeliveryStore::~DeliveryStore() = default;

void DeliveryStore::init_schema() {
    DbGuard db{open_db(db_path_)};
    exec_sql(db.db,
        "CREATE TABLE IF NOT EXISTS deliveries ("
        "  id TEXT PRIMARY KEY,"
        "  kind TEXT NOT NULL DEFAULT 'wake',"
        "  thread_id TEXT NOT NULL,"
        "  target_agent_id TEXT NOT NULL,"
        "  sender_agent_id TEXT NOT NULL DEFAULT '',"
        "  sender_label TEXT NOT NULL DEFAULT '',"
        "  source TEXT NOT NULL DEFAULT 'unknown',"
        "  body TEXT NOT NULL DEFAULT '',"
        "  generation INTEGER NOT NULL DEFAULT 0,"
        "  state TEXT NOT NULL DEFAULT 'queued',"
        "  task_id TEXT NOT NULL DEFAULT '',"
        "  source_message_id TEXT NOT NULL DEFAULT '',"
        "  error TEXT NOT NULL DEFAULT '',"
        "  created_at TEXT NOT NULL,"
        "  dispatched_at TEXT NOT NULL DEFAULT '',"
        "  completed_at TEXT NOT NULL DEFAULT ''"
        ")", "create deliveries");
    // 2026-10-04: message-header source column (same guard pattern as
    // ThreadStore's attachments_json migration). Old rows default to
    // 'unknown'.
    {
        bool has_source = false;
        sqlite3_stmt* st = nullptr;
        if (sqlite3_prepare_v2(db.db, "PRAGMA table_info(deliveries)",
                               -1, &st, nullptr) == SQLITE_OK) {
            while (sqlite3_step(st) == SQLITE_ROW) {
                const char* n = col_text(st, 1);
                if (n && std::string_view{n} == "source") {
                    has_source = true; break;
                }
            }
            sqlite3_finalize(st);
        }
        if (!has_source)
            exec_sql(db.db,
                "ALTER TABLE deliveries ADD COLUMN source"
                " TEXT NOT NULL DEFAULT 'unknown'",
                "migrate deliveries source");
    }
    exec_sql(db.db,
        "CREATE INDEX IF NOT EXISTS idx_deliveries_target"
        " ON deliveries(target_agent_id, state, created_at)",
        "index deliveries target");
    exec_sql(db.db,
        "CREATE INDEX IF NOT EXISTS idx_deliveries_sender"
        " ON deliveries(sender_agent_id, created_at)",
        "index deliveries sender");
    exec_sql(db.db,
        "CREATE TABLE IF NOT EXISTS relay_links ("
        "  pair_thread_id TEXT PRIMARY KEY,"
        "  requester_id TEXT NOT NULL,"
        "  home_thread_id TEXT NOT NULL,"
        "  consumed INTEGER NOT NULL DEFAULT 0,"
        "  created_at TEXT NOT NULL"
        ")", "create relay_links");
    exec_sql(db.db,
        "CREATE TABLE IF NOT EXISTS thread_reads ("
        "  thread_id TEXT NOT NULL,"
        "  agent_id TEXT NOT NULL,"
        "  last_read_message_id TEXT NOT NULL DEFAULT '',"
        "  last_read_at TEXT NOT NULL,"
        "  updated_at TEXT NOT NULL,"
        "  PRIMARY KEY (thread_id, agent_id)"
        ")", "create thread_reads");
}

// ── deliveries ───────────────────────────────────────────────────────────

std::string DeliveryStore::enqueue(Delivery d) {
    // 2026-10-22 (bug sweep B6): the delivery body rides worker frames
    // as JSON — repair it here (the single enqueue choke point for every
    // wake) so a poison body can never reach WorkerChannel::send's dump.
    d.body = hivemind::sanitize_utf8(d.body);
    d.sender_label = hivemind::sanitize_utf8(d.sender_label);
    if (d.id.empty()) d.id = uuid_v4();
    if (d.state.empty()) d.state = "queued";
    if (d.kind.empty()) d.kind = "wake";
    if (d.created_at.empty()) d.created_at = iso_now();
    if (d.source.empty()) d.source = "unknown";
    DbGuard db{open_db(db_path_)};
    StmtGuard st;
    const std::string sql = "INSERT INTO deliveries (" +
        std::string(kCols) + ") VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)";
    prepare(db.db, st, sql.c_str(), "enqueue");
    bind_text(st, 1, d.id);
    bind_text(st, 2, d.kind);
    bind_text(st, 3, d.thread_id);
    bind_text(st, 4, d.target_agent_id);
    bind_text(st, 5, d.sender_agent_id);
    bind_text(st, 6, d.sender_label);
    bind_text(st, 7, d.source);
    bind_text(st, 8, d.body);
    sqlite3_bind_int(st, 9, d.generation);
    bind_text(st, 10, d.state);
    bind_text(st, 11, d.task_id);
    bind_text(st, 12, d.source_message_id);
    bind_text(st, 13, d.error);
    bind_text(st, 14, d.created_at);
    bind_text(st, 15, d.dispatched_at);
    bind_text(st, 16, d.completed_at);
    step_done(db.db, st, "enqueue");
    return d.id;
}

std::optional<Delivery> DeliveryStore::get(const std::string& id) {
    DbGuard db{open_db(db_path_)};
    StmtGuard st;
    prepare(db.db, st,
        ("SELECT " + std::string(kCols) +
         " FROM deliveries WHERE id = ?").c_str(), "get");
    bind_text(st, 1, id);
    if (sqlite3_step(st) != SQLITE_ROW) return std::nullopt;
    return row_to_delivery(st);
}

std::optional<Delivery>
DeliveryStore::oldest_queued(const std::string& target_agent_id) {
    DbGuard db{open_db(db_path_)};
    StmtGuard st;
    prepare(db.db, st,
        ("SELECT " + std::string(kCols) +
         " FROM deliveries WHERE target_agent_id = ? AND state = 'queued'"
         " ORDER BY created_at ASC, rowid ASC LIMIT 1").c_str(),
        "oldest_queued");
    bind_text(st, 1, target_agent_id);
    if (sqlite3_step(st) != SQLITE_ROW) return std::nullopt;
    return row_to_delivery(st);
}

bool DeliveryStore::has_live_wake(const std::string& target_agent_id,
                                  const std::string& source_message_id) {
    if (source_message_id.empty()) return false;
    DbGuard db{open_db(db_path_)};
    StmtGuard st;
    prepare(db.db, st,
        "SELECT COUNT(*) FROM deliveries"
        " WHERE target_agent_id = ? AND source_message_id = ?"
        " AND state IN ('queued','dispatched')", "has_live_wake");
    bind_text(st, 1, target_agent_id);
    bind_text(st, 2, source_message_id);
    if (sqlite3_step(st) != SQLITE_ROW) return false;
    return sqlite3_column_int(st, 0) > 0;
}

void DeliveryStore::mark_dispatched(const std::string& id,
                                    const std::string& task_id) {
    DbGuard db{open_db(db_path_)};
    StmtGuard st;
    prepare(db.db, st,
        "UPDATE deliveries SET state='dispatched', task_id=?,"
        " dispatched_at=? WHERE id=? AND state='queued'",
        "mark_dispatched");
    bind_text(st, 1, task_id);
    bind_text(st, 2, iso_now());
    bind_text(st, 3, id);
    step_done(db.db, st, "mark_dispatched");
}

std::optional<Delivery>
DeliveryStore::find_by_task(const std::string& task_id) {
    if (task_id.empty()) return std::nullopt;
    DbGuard db{open_db(db_path_)};
    StmtGuard st;
    prepare(db.db, st,
        ("SELECT " + std::string(kCols) +
         " FROM deliveries WHERE task_id = ?"
         " ORDER BY rowid DESC LIMIT 1").c_str(), "find_by_task");
    bind_text(st, 1, task_id);
    if (sqlite3_step(st) != SQLITE_ROW) return std::nullopt;
    return row_to_delivery(st);
}

std::vector<hivemind::Delivery>
DeliveryStore::find_all_by_task(const std::string& task_id) {
    std::vector<Delivery> out;
    if (task_id.empty()) return out;
    DbGuard db{open_db(db_path_)};
    StmtGuard st;
    prepare(db.db, st,
        ("SELECT " + std::string(kCols) +
         " FROM deliveries WHERE task_id = ?"
         " ORDER BY rowid ASC").c_str(), "find_all_by_task");
    bind_text(st, 1, task_id);
    while (sqlite3_step(st) == SQLITE_ROW) out.push_back(row_to_delivery(st));
    return out;
}

std::vector<hivemind::Delivery>
DeliveryStore::queued_for_thread(const std::string& target_agent_id,
                                 const std::string& thread_id,
                                 int generation, int limit) {
    std::vector<Delivery> out;
    DbGuard db{open_db(db_path_)};
    StmtGuard st;
    prepare(db.db, st,
        ("SELECT " + std::string(kCols) +
         " FROM deliveries WHERE target_agent_id = ? AND thread_id = ?"
         " AND state = 'queued' AND generation = ?"
         " ORDER BY created_at ASC, rowid ASC LIMIT ?").c_str(),
        "queued_for_thread");
    bind_text(st, 1, target_agent_id);
    bind_text(st, 2, thread_id);
    sqlite3_bind_int(st, 3, generation);
    sqlite3_bind_int(st, 4, limit);
    while (sqlite3_step(st) == SQLITE_ROW) out.push_back(row_to_delivery(st));
    return out;
}

void DeliveryStore::mark_done(const std::string& id) {
    DbGuard db{open_db(db_path_)};
    StmtGuard st;
    prepare(db.db, st,
        "UPDATE deliveries SET state='done', completed_at=?"
        " WHERE id=? AND state='dispatched'", "mark_done");
    bind_text(st, 1, iso_now());
    bind_text(st, 2, id);
    step_done(db.db, st, "mark_done");
}

void DeliveryStore::mark_error(const std::string& id,
                               const std::string& error) {
    DbGuard db{open_db(db_path_)};
    StmtGuard st;
    prepare(db.db, st,
        "UPDATE deliveries SET state='error', error=?, completed_at=?"
        " WHERE id=?", "mark_error");
    bind_text(st, 1, error);
    bind_text(st, 2, iso_now());
    bind_text(st, 3, id);
    step_done(db.db, st, "mark_error");
}

void DeliveryStore::mark_dropped(const std::string& id,
                                 const std::string& reason) {
    DbGuard db{open_db(db_path_)};
    StmtGuard st;
    prepare(db.db, st,
        "UPDATE deliveries SET state='dropped', error=?, completed_at=?"
        " WHERE id=?", "mark_dropped");
    bind_text(st, 1, reason);
    bind_text(st, 2, iso_now());
    bind_text(st, 3, id);
    step_done(db.db, st, "mark_dropped");
}

int DeliveryStore::expire_queued(int ttl_s) {
    DbGuard db{open_db(db_path_)};
    StmtGuard st;
    prepare(db.db, st,
        "UPDATE deliveries SET state='expired', error='wake ttl',"
        " completed_at=? WHERE state='queued' AND created_at < ?",
        "expire_queued");
    bind_text(st, 1, iso_now());
    bind_text(st, 2, hivemind::db::iso_ago(ttl_s));
    step_done(db.db, st, "expire_queued");
    return sqlite3_changes(db.db);
}

int DeliveryStore::requeue_orphans() {
    DbGuard db{open_db(db_path_)};
    StmtGuard st;
    prepare(db.db, st,
        "UPDATE deliveries SET state='queued', task_id='',"
        " dispatched_at='' WHERE state='dispatched'", "requeue_orphans");
    step_done(db.db, st, "requeue_orphans");
    return sqlite3_changes(db.db);
}

std::vector<Delivery> DeliveryStore::list(const std::string& agent_id,
                                          const std::string& state,
                                          int limit) {
    std::vector<Delivery> out;
    DbGuard db{open_db(db_path_)};
    std::string sql = "SELECT " + std::string(kCols) + " FROM deliveries";
    std::string where;
    if (!agent_id.empty())
        where += "(target_agent_id = ? OR sender_agent_id = ?)";
    if (!state.empty()) {
        if (!where.empty()) where += " AND ";
        where += "state = ?";
    }
    if (!where.empty()) sql += " WHERE " + where;
    sql += " ORDER BY created_at DESC, rowid DESC LIMIT ?";
    StmtGuard st;
    prepare(db.db, st, sql.c_str(), "list");
    int idx = 1;
    if (!agent_id.empty()) { bind_text(st, idx++, agent_id);
                             bind_text(st, idx++, agent_id); }
    if (!state.empty()) bind_text(st, idx++, state);
    sqlite3_bind_int(st, idx, limit);
    while (sqlite3_step(st) == SQLITE_ROW)
        out.push_back(row_to_delivery(st));
    return out;
}

std::vector<Delivery> DeliveryStore::sent_by(const std::string& sender_id,
                                             int limit) {
    std::vector<Delivery> out;
    DbGuard db{open_db(db_path_)};
    StmtGuard st;
    prepare(db.db, st,
        ("SELECT " + std::string(kCols) +
         " FROM deliveries WHERE sender_agent_id = ?"
         " ORDER BY created_at DESC, rowid DESC LIMIT ?").c_str(),
        "sent_by");
    bind_text(st, 1, sender_id);
    sqlite3_bind_int(st, 2, limit);
    while (sqlite3_step(st) == SQLITE_ROW)
        out.push_back(row_to_delivery(st));
    return out;
}

int DeliveryStore::prune_terminal_older_than(int days) {
    DbGuard db{open_db(db_path_)};
    StmtGuard st;
    prepare(db.db, st,
        "DELETE FROM deliveries WHERE state IN"
        " ('done','error','dropped','expired')"
        " AND completed_at != '' AND completed_at < ?",
        "prune_terminal");
    bind_text(st, 1, hivemind::db::iso_ago(
                       static_cast<int64_t>(days) * 86400));
    step_done(db.db, st, "prune_terminal");
    return sqlite3_changes(db.db);
}

// ── relay links ──────────────────────────────────────────────────────────

void DeliveryStore::arm_relay(const std::string& pair_thread_id,
                              const std::string& requester_id,
                              const std::string& home_thread_id) {
    DbGuard db{open_db(db_path_)};
    StmtGuard st;
    prepare(db.db, st,
        "INSERT INTO relay_links"
        " (pair_thread_id, requester_id, home_thread_id, consumed,"
        "  created_at)"
        " VALUES (?,?,?,0,?)"
        " ON CONFLICT(pair_thread_id) DO UPDATE SET"
        " requester_id=excluded.requester_id,"
        " home_thread_id=excluded.home_thread_id,"
        " consumed=0, created_at=excluded.created_at",
        "arm_relay");
    bind_text(st, 1, pair_thread_id);
    bind_text(st, 2, requester_id);
    bind_text(st, 3, home_thread_id);
    bind_text(st, 4, iso_now());
    step_done(db.db, st, "arm_relay");
}

std::optional<RelayLinkRow>
DeliveryStore::consume_relay(const std::string& pair_thread_id,
                             const std::string& replier_id) {
    DbGuard db{open_db(db_path_)};
    RelayLinkRow row;
    bool found = false;
    {
        StmtGuard st;
        prepare(db.db, st,
            "SELECT requester_id, home_thread_id, created_at"
            " FROM relay_links"
            " WHERE pair_thread_id = ? AND consumed = 0",
            "consume_relay select");
        bind_text(st, 1, pair_thread_id);
        if (sqlite3_step(st) == SQLITE_ROW) {
            row.pair_thread_id = pair_thread_id;
            row.requester_id   = col_text(st, 0);
            row.home_thread_id = col_text(st, 1);
            row.created_at     = col_text(st, 2);
            row.consumed       = false;
            found = true;
        }
    }
    // The requester's own messages in the pair thread never relay; the
    // link stays armed for the peer's reply.
    if (!found || row.requester_id == replier_id) return std::nullopt;
    StmtGuard st;
    prepare(db.db, st,
        "UPDATE relay_links SET consumed = 1"
        " WHERE pair_thread_id = ? AND consumed = 0",
        "consume_relay update");
    bind_text(st, 1, pair_thread_id);
    step_done(db.db, st, "consume_relay update");
    // 2026-10-22 (bug sweep 2): the consume must be a CLAIM, not a
    // blind write. Two replier agents finishing turns in the same pair
    // thread concurrently could both read consumed=0 and both fire the
    // relay. Only the UPDATE that actually flipped the flag wins; the
    // loser's earlier SELECT is stale.
    if (sqlite3_changes(db.db) != 1) return std::nullopt;
    return row;
}

int DeliveryStore::expire_relays(int ttl_s) {
    DbGuard db{open_db(db_path_)};
    StmtGuard st;
    prepare(db.db, st,
        "DELETE FROM relay_links WHERE created_at < ?",
        "expire_relays");
    bind_text(st, 1, hivemind::db::iso_ago(ttl_s));
    step_done(db.db, st, "expire_relays");
    return sqlite3_changes(db.db);
}

// ── thread read high-water marks ─────────────────────────────────────────

void DeliveryStore::mark_read(const std::string& thread_id,
                              const std::string& agent_id,
                              const std::string& message_id,
                              const std::string& message_created_at) {
    DbGuard db{open_db(db_path_)};
    StmtGuard st;
    // Monotonic advance only — an older message never drags the mark back.
    prepare(db.db, st,
        "INSERT INTO thread_reads"
        " (thread_id, agent_id, last_read_message_id, last_read_at,"
        "  updated_at)"
        " VALUES (?,?,?,?,?)"
        " ON CONFLICT(thread_id, agent_id) DO UPDATE SET"
        " last_read_message_id=excluded.last_read_message_id,"
        " last_read_at=excluded.last_read_at,"
        " updated_at=excluded.updated_at"
        " WHERE excluded.last_read_at > thread_reads.last_read_at",
        "mark_read");
    bind_text(st, 1, thread_id);
    bind_text(st, 2, agent_id);
    bind_text(st, 3, message_id);
    bind_text(st, 4, message_created_at);
    bind_text(st, 5, iso_now());
    step_done(db.db, st, "mark_read");
}

std::optional<std::string>
DeliveryStore::read_mark(const std::string& thread_id,
                         const std::string& agent_id) {
    DbGuard db{open_db(db_path_)};
    StmtGuard st;
    prepare(db.db, st,
        "SELECT last_read_at FROM thread_reads"
        " WHERE thread_id = ? AND agent_id = ?", "read_mark");
    bind_text(st, 1, thread_id);
    bind_text(st, 2, agent_id);
    if (sqlite3_step(st) != SQLITE_ROW) return std::nullopt;
    return std::string{col_text(st, 0)};
}

}  // namespace hivemind
