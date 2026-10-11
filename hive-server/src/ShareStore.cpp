// =============================================================================
// ShareStore (2026-10-11, Plan.md "Agent-to-Agent Secure File Sharing"):
// durable metadata registry for addressed agent-to-agent file shares.
// See hivemind/ShareStore.h for the contract.
// =============================================================================

#include "hivemind/ShareStore.h"
#include "hivemind/DbUtil.h"

#include <sqlite3.h>

#include <cstdlib>
#include <stdexcept>

using hivemind::db::bind_text;
using hivemind::db::col_text;
using hivemind::db::exec_sql;
using hivemind::db::iso_now;

namespace fs = std::filesystem;

namespace {

sqlite3* open_db(const fs::path& p) {
    sqlite3* db = nullptr;
    if (sqlite3_open(p.string().c_str(), &db) != SQLITE_OK) {
        std::string msg = db ? sqlite3_errmsg(db) : "out of memory";
        if (db) sqlite3_close(db);
        throw std::runtime_error("ShareStore: cannot open " +
                                 p.string() + ": " + msg);
    }
    // Wait up to 5s on a contended write lock instead of failing
    // instantly (the 2026-10-01 "database is locked" lesson).
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
        throw std::runtime_error(std::string("ShareStore: prepare ") +
                                 what + ": " + sqlite3_errmsg(db));
}

void step_done(sqlite3* db, sqlite3_stmt* st, const char* what) {
    if (sqlite3_step(st) != SQLITE_DONE)
        throw std::runtime_error(std::string("ShareStore: ") + what +
                                 ": " + sqlite3_errmsg(db));
}

constexpr const char* kCols =
    "id, sender_agent_id, target_agent_id, filename, size_bytes, note,"
    " worker, rel_path, state, created_at, fetched_at, fetch_count";

hivemind::Share row_to_share(sqlite3_stmt* st) {
    hivemind::Share s;
    s.id              = col_text(st, 0);
    s.sender_agent_id = col_text(st, 1);
    s.target_agent_id = col_text(st, 2);
    s.filename        = col_text(st, 3);
    s.size_bytes      = sqlite3_column_int64(st, 4);
    s.note            = col_text(st, 5);
    s.worker          = col_text(st, 6);
    s.rel_path        = col_text(st, 7);
    s.state           = col_text(st, 8);
    s.created_at      = col_text(st, 9);
    s.fetched_at      = col_text(st, 10);
    s.fetch_count     = sqlite3_column_int(st, 11);
    return s;
}

}  // namespace

namespace hivemind {

ShareStore::ShareStore() {
    db_path_ = fs::path{std::getenv("HOME") ? std::getenv("HOME") : "."}
               / ".local" / "share" / "hivemind" / "shares.db";
    fs::create_directories(db_path_.parent_path());
    init_schema();
}

ShareStore::ShareStore(fs::path db) : db_path_(std::move(db)) {
    fs::create_directories(db_path_.parent_path());
    init_schema();
}

ShareStore::~ShareStore() = default;

void ShareStore::init_schema() {
    DbGuard db{open_db(db_path_)};
    exec_sql(db.db,
        "CREATE TABLE IF NOT EXISTS shares ("
        "  id TEXT PRIMARY KEY,"
        "  sender_agent_id TEXT NOT NULL,"
        "  target_agent_id TEXT NOT NULL,"
        "  filename TEXT NOT NULL,"
        "  size_bytes INTEGER NOT NULL DEFAULT 0,"
        "  note TEXT NOT NULL DEFAULT '',"
        "  worker TEXT NOT NULL,"
        "  rel_path TEXT NOT NULL,"
        "  state TEXT NOT NULL DEFAULT 'active',"
        "  created_at TEXT NOT NULL,"
        "  fetched_at TEXT NOT NULL DEFAULT '',"
        "  fetch_count INTEGER NOT NULL DEFAULT 0"
        ")",
        "ShareStore");
    exec_sql(db.db,
        "CREATE INDEX IF NOT EXISTS idx_shares_target"
        "  ON shares(target_agent_id, state)",
        "ShareStore");
    exec_sql(db.db,
        "CREATE INDEX IF NOT EXISTS idx_shares_sender"
        "  ON shares(sender_agent_id)",
        "ShareStore");
}

void ShareStore::create_share(Share s) {
    if (s.id.empty() || s.sender_agent_id.empty() ||
        s.target_agent_id.empty() || s.filename.empty() ||
        s.worker.empty() || s.rel_path.empty())
        throw std::invalid_argument(
            "ShareStore: create_share requires id, sender, target,"
            " filename, worker and rel_path");
    if (s.size_bytes < 0)
        throw std::invalid_argument("ShareStore: negative size_bytes");
    DbGuard db{open_db(db_path_)};
    StmtGuard st;
    const std::string sql =
        "INSERT INTO shares (" + std::string{kCols} +
        ") VALUES (?,?,?,?,?,?,?,?,'active',?,'',0)";
    prepare(db.db, st, sql.c_str(), "insert");
    bind_text(st, 1, s.id);
    bind_text(st, 2, s.sender_agent_id);
    bind_text(st, 3, s.target_agent_id);
    bind_text(st, 4, s.filename);
    sqlite3_bind_int64(st, 5, s.size_bytes);
    bind_text(st, 6, s.note);
    bind_text(st, 7, s.worker);
    bind_text(st, 8, s.rel_path);
    bind_text(st, 9, iso_now());
    step_done(db.db, st, "insert share");
}

std::optional<Share> ShareStore::get_share(const std::string& id) {
    DbGuard db{open_db(db_path_)};
    StmtGuard st;
    const std::string sql =
        "SELECT " + std::string{kCols} + " FROM shares WHERE id=?";
    prepare(db.db, st, sql.c_str(), "get");
    bind_text(st, 1, id);
    if (sqlite3_step(st) != SQLITE_ROW) return std::nullopt;
    return row_to_share(st);
}

std::vector<Share> ShareStore::list_for_agent(const std::string& agent_id,
                                              int limit) {
    DbGuard db{open_db(db_path_)};
    StmtGuard st;
    std::string sql = "SELECT " + std::string{kCols} + " FROM shares";
    if (!agent_id.empty())
        sql += " WHERE sender_agent_id=? OR target_agent_id=?";
    sql += " ORDER BY created_at DESC, rowid DESC LIMIT ?";
    prepare(db.db, st, sql.c_str(), "list");
    if (!agent_id.empty()) {
        bind_text(st, 1, agent_id);
        bind_text(st, 2, agent_id);
        sqlite3_bind_int(st, 3, limit);
    } else {
        sqlite3_bind_int(st, 1, limit);
    }
    std::vector<Share> out;
    while (sqlite3_step(st) == SQLITE_ROW)
        out.push_back(row_to_share(st));
    return out;
}

void ShareStore::mark_fetched(const std::string& id) {
    DbGuard db{open_db(db_path_)};
    StmtGuard st;
    prepare(db.db, st,
        "UPDATE shares SET fetched_at=?, fetch_count=fetch_count+1"
        " WHERE id=?",
        "mark_fetched");
    bind_text(st, 1, iso_now());
    bind_text(st, 2, id);
    step_done(db.db, st, "mark_fetched");
}

bool ShareStore::revoke(const std::string& id) {
    DbGuard db{open_db(db_path_)};
    StmtGuard st;
    prepare(db.db, st,
        "UPDATE shares SET state='revoked' WHERE id=? AND state='active'",
        "revoke");
    bind_text(st, 1, id);
    step_done(db.db, st, "revoke");
    return sqlite3_changes(db.db) > 0;
}

nlohmann::json share_to_json(const Share& s) {
    return {
        {"share_id", s.id},
        {"sender_agent_id", s.sender_agent_id},
        {"target_agent_id", s.target_agent_id},
        {"filename", s.filename},
        {"size_bytes", s.size_bytes},
        {"note", s.note},
        {"worker", s.worker},
        {"rel_path", s.rel_path},
        {"state", s.state},
        {"created_at", s.created_at},
        {"fetched_at", s.fetched_at},
        {"fetch_count", s.fetch_count},
    };
}

} // namespace hivemind
