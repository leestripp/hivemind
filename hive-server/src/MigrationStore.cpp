// =============================================================================
// MigrationStore — see include/hivemind/MigrationStore.h (Phase 73, Plan.md).
// =============================================================================

#include "hivemind/MigrationStore.h"

#include <sqlite3.h>
#include <stdexcept>
#include <utility>

#include "hivemind/DbUtil.h"

namespace fs = std::filesystem;

namespace hivemind {

using namespace db;  // DbUtil helpers (uuid_v4, iso_now, bind_text, col_text, exec_sql)

namespace {

sqlite3* open_db(const fs::path& p) {
    sqlite3* db = nullptr;
    if (sqlite3_open(p.string().c_str(), &db) != SQLITE_OK) {
        std::string msg = db ? sqlite3_errmsg(db) : "unknown";
        if (db) sqlite3_close(db);
        throw std::runtime_error("MigrationStore: cannot open " +
                                 p.string() + ": " + msg);
    }
    sqlite3_busy_timeout(db, 5000);
    return db;
}

Migration row_to_migration(sqlite3_stmt* st) {
    Migration m;
    m.id            = col_text(st, 0);
    m.agent_id      = col_text(st, 1);
    m.agent_name    = col_text(st, 2);
    m.source_worker = col_text(st, 3);
    m.target_worker = col_text(st, 4);
    m.target_type   = col_text(st, 5);
    m.source_type   = col_text(st, 6);
    m.state         = col_text(st, 7);
    m.error         = col_text(st, 8);
    m.retryable     = sqlite3_column_int(st, 9) != 0;
    m.force         = sqlite3_column_int(st, 10) != 0;
    m.no_rollback   = sqlite3_column_int(st, 11) != 0;
    m.export_path   = col_text(st, 12);
    m.export_size   = sqlite3_column_int64(st, 13);
    m.progress      = col_text(st, 14);
    m.created_at    = col_text(st, 15);
    m.updated_at    = col_text(st, 16);
    return m;
}

constexpr const char* kSelectCols =
    "SELECT id, agent_id, agent_name, source_worker, target_worker,"
    "       target_type, source_type, state, error, retryable, force,"
    "       no_rollback, export_path, export_size, progress, created_at,"
    "       updated_at FROM migrations ";

}  // namespace

MigrationStore::MigrationStore() {
    db_path_ = fs::path{std::getenv("HOME") ? std::getenv("HOME") : "."}
               / ".local" / "share" / "hivemind" / "migrations.db";
    fs::create_directories(db_path_.parent_path());
    init_schema();
}

MigrationStore::MigrationStore(fs::path db) : db_path_(std::move(db)) {
    fs::create_directories(db_path_.parent_path());
    init_schema();
}

MigrationStore::~MigrationStore() = default;

void MigrationStore::init_schema() {
    sqlite3* db = open_db(db_path_);
    exec_sql(db,
             "CREATE TABLE IF NOT EXISTS migrations ("
             "  id TEXT PRIMARY KEY,"
             "  agent_id TEXT NOT NULL,"
             "  agent_name TEXT NOT NULL,"
             "  source_worker TEXT NOT NULL,"
             "  target_worker TEXT NOT NULL,"
             "  target_type TEXT NOT NULL,"
             "  state TEXT NOT NULL,"
             "  error TEXT NOT NULL DEFAULT '',"
             "  retryable INTEGER NOT NULL DEFAULT 0,"
             "  force INTEGER NOT NULL DEFAULT 0,"
             "  no_rollback INTEGER NOT NULL DEFAULT 0,"
             "  export_path TEXT NOT NULL DEFAULT '',"
             "  export_size INTEGER NOT NULL DEFAULT 0,"
             "  progress TEXT NOT NULL DEFAULT '',"
             "  created_at TEXT NOT NULL,"
             "  updated_at TEXT NOT NULL"
             ");"
             // 2026-10-24 (Phase 73 mid-build): original runtime type, for
             // rollback. Additive — ignore the error on existing DBs.
             "ALTER TABLE migrations ADD COLUMN source_type TEXT NOT NULL DEFAULT '';"
             "CREATE INDEX IF NOT EXISTS idx_migrations_agent ON migrations(agent_id);"
             "CREATE INDEX IF NOT EXISTS idx_migrations_state ON migrations(state);",
             "MigrationStore schema");
    sqlite3_close(db);
}

bool MigrationStore::is_live(const std::string& state) {
    // Terminal states release the per-agent slot and exit the sweeper.
    return state != "done" && state != "failed" && state != "cancelled" &&
           state != "rolled_back";
}

std::string MigrationStore::create(Migration m) {
    if (m.agent_id.empty()) throw std::invalid_argument("migration: agent_id empty");
    if (m.source_worker.empty() || m.target_worker.empty())
        throw std::invalid_argument("migration: source/target worker empty");
    if (m.target_type != "container" && m.target_type != "host")
        throw std::invalid_argument("migration: target_type must be container|host");
    if (m.source_type.empty()) m.source_type = "container";
    if (m.source_worker == m.target_worker && m.source_type == m.target_type)
        throw std::invalid_argument("migration: agent already runs that runtime there");

    sqlite3* db = open_db(db_path_);
    // One live job per agent (fresh connection per op — DbUtil pattern).
    {
        const char* sql =
            "SELECT COUNT(*) FROM migrations WHERE agent_id = ? AND"
            "  state NOT IN ('done','failed','cancelled','rolled_back')";
        sqlite3_stmt* st = nullptr;
        sqlite3_prepare_v2(db, sql, -1, &st, nullptr);
        bind_text(st, 1, m.agent_id);
        if (sqlite3_step(st) == SQLITE_ROW && sqlite3_column_int(st, 0) > 0) {
            sqlite3_finalize(st);
            sqlite3_close(db);
            throw std::invalid_argument("migration: live job already exists for agent");
        }
        sqlite3_finalize(st);
    }

    if (m.id.empty()) m.id = uuid_v4();
    if (m.state.empty()) m.state = "pending";
    m.created_at = iso_now();
    m.updated_at = m.created_at;

    const char* sql =
        "INSERT INTO migrations (id, agent_id, agent_name, source_worker,"
        "  target_worker, target_type, source_type, state, error, retryable,"
        "  force, no_rollback, export_path, export_size, progress, created_at,"
        "  updated_at) VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)";
    sqlite3_stmt* st = nullptr;
    sqlite3_prepare_v2(db, sql, -1, &st, nullptr);
    bind_text(st, 1, m.id);
    bind_text(st, 2, m.agent_id);
    bind_text(st, 3, m.agent_name);
    bind_text(st, 4, m.source_worker);
    bind_text(st, 5, m.target_worker);
    bind_text(st, 6, m.target_type);
    bind_text(st, 7, m.source_type);
    bind_text(st, 8, m.state);
    bind_text(st, 9, m.error);
    sqlite3_bind_int(st, 10, m.retryable ? 1 : 0);
    sqlite3_bind_int(st, 11, m.force ? 1 : 0);
    sqlite3_bind_int(st, 12, m.no_rollback ? 1 : 0);
    bind_text(st, 13, m.export_path);
    sqlite3_bind_int64(st, 14, m.export_size);
    bind_text(st, 15, m.progress);
    bind_text(st, 16, m.created_at);
    bind_text(st, 17, m.updated_at);
    if (sqlite3_step(st) != SQLITE_DONE) {
        std::string msg = sqlite3_errmsg(db);
        sqlite3_finalize(st);
        sqlite3_close(db);
        throw std::runtime_error("MigrationStore insert failed: " + msg);
    }
    sqlite3_finalize(st);
    sqlite3_close(db);
    return m.id;
}

std::optional<Migration> MigrationStore::get(const std::string& id) {
    sqlite3* db = open_db(db_path_);
    std::string sql = std::string(kSelectCols) + "WHERE id = ?";
    sqlite3_stmt* st = nullptr;
    sqlite3_prepare_v2(db, sql.c_str(), -1, &st, nullptr);
    bind_text(st, 1, id);
    std::optional<Migration> out;
    if (sqlite3_step(st) == SQLITE_ROW) out = row_to_migration(st);
    sqlite3_finalize(st);
    sqlite3_close(db);
    return out;
}

std::vector<Migration> MigrationStore::list(const std::string& agent_id, int limit) {
    sqlite3* db = open_db(db_path_);
    std::string sql = std::string(kSelectCols);
    if (!agent_id.empty()) sql += "WHERE agent_id = ? ";
    sql += "ORDER BY created_at DESC, rowid DESC LIMIT ?";
    sqlite3_stmt* st = nullptr;
    sqlite3_prepare_v2(db, sql.c_str(), -1, &st, nullptr);
    int idx = 1;
    if (!agent_id.empty()) bind_text(st, idx++, agent_id);
    sqlite3_bind_int(st, idx, limit);
    std::vector<Migration> out;
    while (sqlite3_step(st) == SQLITE_ROW) out.push_back(row_to_migration(st));
    sqlite3_finalize(st);
    sqlite3_close(db);
    return out;
}

void MigrationStore::set_state(const std::string& id, const std::string& state,
                               const std::string& error, const std::string& progress) {
    sqlite3* db = open_db(db_path_);
    const char* sql =
        "UPDATE migrations SET state = ?, error = ?,"
        "  progress = CASE WHEN ? <> '' THEN ? ELSE progress END,"
        "  updated_at = ? WHERE id = ?";
    sqlite3_stmt* st = nullptr;
    sqlite3_prepare_v2(db, sql, -1, &st, nullptr);
    bind_text(st, 1, state);
    bind_text(st, 2, error);
    bind_text(st, 3, progress);
    bind_text(st, 4, progress);
    bind_text(st, 5, iso_now());
    bind_text(st, 6, id);
    sqlite3_step(st);
    sqlite3_finalize(st);
    sqlite3_close(db);
}

void MigrationStore::set_export(const std::string& id, const std::string& path,
                                long long size) {
    sqlite3* db = open_db(db_path_);
    const char* sql =
        "UPDATE migrations SET export_path = ?, export_size = ?, updated_at = ?"
        " WHERE id = ?";
    sqlite3_stmt* st = nullptr;
    sqlite3_prepare_v2(db, sql, -1, &st, nullptr);
    bind_text(st, 1, path);
    sqlite3_bind_int64(st, 2, size);
    bind_text(st, 3, iso_now());
    bind_text(st, 4, id);
    sqlite3_step(st);
    sqlite3_finalize(st);
    sqlite3_close(db);
}

std::optional<Migration> MigrationStore::live_job_for(const std::string& agent_id) {
    for (const auto& m : list(agent_id, 100)) {
        if (is_live(m.state)) return m;
    }
    return std::nullopt;
}

std::vector<Migration> MigrationStore::inflight() {
    sqlite3* db = open_db(db_path_);
    std::string sql = std::string(kSelectCols) +
        "WHERE state IN ('pending','exporting','transferring','provisioning',"
        "  'importing','verifying','cleanup')"
        " ORDER BY created_at ASC, rowid ASC";
    sqlite3_stmt* st = nullptr;
    sqlite3_prepare_v2(db, sql.c_str(), -1, &st, nullptr);
    std::vector<Migration> out;
    while (sqlite3_step(st) == SQLITE_ROW) out.push_back(row_to_migration(st));
    sqlite3_finalize(st);
    sqlite3_close(db);
    return out;
}

}  // namespace hivemind
