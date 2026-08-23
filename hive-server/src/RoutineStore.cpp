// =============================================================================
// src/RoutineStore.cpp — agent routines (HiveMind 2.0, Plan.md Phase 5)
// =============================================================================
#include "hivemind/RoutineStore.h"

#include <array>
#include <chrono>
#include <cstdio>
#include <stdexcept>

namespace fs = std::filesystem;

namespace {

std::string uuid_v4() {
    std::array<unsigned char, 16> buf{};
    FILE* f = std::fopen("/dev/urandom", "rb");
    if (f) { std::fread(buf.data(), 1, 16, f); std::fclose(f); }
    char out[37];
    std::snprintf(out, sizeof(out),
        "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
        buf[0],buf[1],buf[2],buf[3], buf[4],buf[5],
        (buf[6] & 0x0f) | 0x40, buf[7],
        (buf[8] & 0x3f) | 0x80, buf[9], buf[10],buf[11],
        buf[12],buf[13],buf[14],buf[15]);
    return out;
}

std::string iso_now() {
    auto now = std::chrono::system_clock::now();
    auto t = std::chrono::system_clock::to_time_t(now);
    char buf[64];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", std::gmtime(&t));
    return buf;
}

inline void bind_text(sqlite3_stmt* st, int idx, const std::string& v) {
    sqlite3_bind_text(st, idx, v.c_str(), -1, SQLITE_TRANSIENT);
}

inline void exec_sql(sqlite3* db, const char* sql) {
    char* err = nullptr;
    if (sqlite3_exec(db, sql, nullptr, nullptr, &err) != SQLITE_OK) {
        std::string msg = err ? err : "unknown";
        sqlite3_free(err);
        throw std::runtime_error("RoutineStore exec failed: " + msg);
    }
}

const char* col_text(sqlite3_stmt* st, int i) {
    const auto* p = sqlite3_column_text(st, i);
    return p ? reinterpret_cast<const char*>(p) : "";
}

Routine row_to_routine(sqlite3_stmt* st) {
    Routine r;
    r.id            = col_text(st, 0);
    r.agent_id      = col_text(st, 1);
    r.thread_id     = col_text(st, 2);
    r.name          = col_text(st, 3);
    r.prompt        = col_text(st, 4);
    r.schedule_json = col_text(st, 5);
    r.next_fire_at  = col_text(st, 6);
    r.enabled       = sqlite3_column_int(st, 7) != 0;
    r.created_at    = col_text(st, 8);
    r.last_fired_at = col_text(st, 9);
    return r;
}

constexpr const char* kCols =
    "SELECT id, agent_id, thread_id, name, prompt, schedule_json,"
    "       next_fire_at, enabled, created_at, last_fired_at FROM routines";

} // namespace

RoutineStore::RoutineStore() {
    db_path_ = fs::path{std::getenv("HOME") ? std::getenv("HOME") : "."}
             / ".local" / "share" / "hivemind" / "routines.db";
    fs::create_directories(db_path_.parent_path());
    ensure_schema();
}

RoutineStore::RoutineStore(fs::path db) : db_path_(std::move(db)) {
    fs::create_directories(db_path_.parent_path());
    ensure_schema();
}

RoutineStore::~RoutineStore() = default;

sqlite3* RoutineStore::open_conn() {
    sqlite3* db = nullptr;
    if (sqlite3_open(db_path_.string().c_str(), &db) != SQLITE_OK) {
        std::string msg = db ? sqlite3_errmsg(db) : "out of memory";
        if (db) sqlite3_close(db);
        throw std::runtime_error("RoutineStore open failed: " + msg);
    }
    return db;
}

void RoutineStore::ensure_schema() {
    sqlite3* db = open_conn();
    exec_sql(db,
        "CREATE TABLE IF NOT EXISTS routines ("
        "  id TEXT PRIMARY KEY,"
        "  agent_id TEXT NOT NULL,"
        "  thread_id TEXT NOT NULL,"
        "  name TEXT NOT NULL,"
        "  prompt TEXT NOT NULL,"
        "  schedule_json TEXT NOT NULL,"
        "  next_fire_at TEXT NOT NULL DEFAULT '',"
        "  enabled INTEGER NOT NULL DEFAULT 1,"
        "  created_at TEXT NOT NULL,"
        "  last_fired_at TEXT NOT NULL DEFAULT ''"
        ")");
    exec_sql(db,
        "CREATE INDEX IF NOT EXISTS idx_routines_due"
        " ON routines(enabled, next_fire_at)");
    sqlite3_close(db);
}

Routine RoutineStore::create(const std::string& agent_id,
                             const std::string& thread_id,
                             const std::string& name,
                             const std::string& prompt,
                             const std::string& schedule_json,
                             const std::string& next_fire_at) {
    Routine r;
    r.id = uuid_v4();
    r.agent_id = agent_id;
    r.thread_id = thread_id;
    r.name = name;
    r.prompt = prompt;
    r.schedule_json = schedule_json;
    r.next_fire_at = next_fire_at;
    r.enabled = true;
    r.created_at = iso_now();

    sqlite3* db = open_conn();
    sqlite3_stmt* st = nullptr;
    const char* sql =
        "INSERT INTO routines (id, agent_id, thread_id, name, prompt,"
        "                     schedule_json, next_fire_at, enabled,"
        "                     created_at, last_fired_at)"
        " VALUES (?,?,?,?,?,?,?,1,?,'')";
    if (sqlite3_prepare_v2(db, sql, -1, &st, nullptr) != SQLITE_OK) {
        std::string msg = sqlite3_errmsg(db);
        sqlite3_close(db);
        throw std::runtime_error("RoutineStore insert prepare failed: " + msg);
    }
    bind_text(st, 1, r.id);
    bind_text(st, 2, r.agent_id);
    bind_text(st, 3, r.thread_id);
    bind_text(st, 4, r.name);
    bind_text(st, 5, r.prompt);
    bind_text(st, 6, r.schedule_json);
    bind_text(st, 7, r.next_fire_at);
    bind_text(st, 8, r.created_at);
    bool ok = sqlite3_step(st) == SQLITE_DONE;
    sqlite3_finalize(st);
    sqlite3_close(db);
    if (!ok) throw std::runtime_error("RoutineStore insert failed");
    return r;
}

std::optional<Routine> RoutineStore::get(const std::string& id) {
    sqlite3* db = open_conn();
    std::optional<Routine> out;
    sqlite3_stmt* st = nullptr;
    std::string sql = std::string{kCols} + " WHERE id = ?";
    if (sqlite3_prepare_v2(db, sql.c_str(), -1, &st, nullptr) == SQLITE_OK) {
        bind_text(st, 1, id);
        if (sqlite3_step(st) == SQLITE_ROW) out = row_to_routine(st);
        sqlite3_finalize(st);
    }
    sqlite3_close(db);
    return out;
}

std::vector<Routine> RoutineStore::list() {
    sqlite3* db = open_conn();
    std::vector<Routine> out;
    sqlite3_stmt* st = nullptr;
    std::string sql = std::string{kCols} + " ORDER BY created_at DESC";
    if (sqlite3_prepare_v2(db, sql.c_str(), -1, &st, nullptr) == SQLITE_OK) {
        while (sqlite3_step(st) == SQLITE_ROW) out.push_back(row_to_routine(st));
        sqlite3_finalize(st);
    }
    sqlite3_close(db);
    return out;
}

std::vector<Routine> RoutineStore::list_due(const std::string& now_iso_local) {
    sqlite3* db = open_conn();
    std::vector<Routine> out;
    sqlite3_stmt* st = nullptr;
    std::string sql = std::string{kCols} +
        " WHERE enabled = 1 AND next_fire_at != '' AND next_fire_at <= ?";
    if (sqlite3_prepare_v2(db, sql.c_str(), -1, &st, nullptr) == SQLITE_OK) {
        bind_text(st, 1, now_iso_local);
        while (sqlite3_step(st) == SQLITE_ROW) out.push_back(row_to_routine(st));
        sqlite3_finalize(st);
    }
    sqlite3_close(db);
    return out;
}

void RoutineStore::mark_fired(const std::string& id, const std::string& fired_at,
                              const std::string& next_fire_at, bool disable) {
    sqlite3* db = open_conn();
    sqlite3_stmt* st = nullptr;
    const char* sql =
        "UPDATE routines SET last_fired_at = ?, next_fire_at = ?,"
        " enabled = CASE WHEN ? THEN 0 ELSE enabled END WHERE id = ?";
    if (sqlite3_prepare_v2(db, sql, -1, &st, nullptr) == SQLITE_OK) {
        bind_text(st, 1, fired_at);
        bind_text(st, 2, next_fire_at);
        sqlite3_bind_int(st, 3, disable ? 1 : 0);
        bind_text(st, 4, id);
        sqlite3_step(st);
        sqlite3_finalize(st);
    }
    sqlite3_close(db);
}

bool RoutineStore::set_enabled(const std::string& id, bool enabled) {
    sqlite3* db = open_conn();
    sqlite3_stmt* st = nullptr;
    bool ok = false;
    if (sqlite3_prepare_v2(db,
            "UPDATE routines SET enabled = ? WHERE id = ?",
            -1, &st, nullptr) == SQLITE_OK) {
        sqlite3_bind_int(st, 1, enabled ? 1 : 0);
        bind_text(st, 2, id);
        ok = sqlite3_step(st) == SQLITE_DONE && sqlite3_changes(db) > 0;
        sqlite3_finalize(st);
    }
    sqlite3_close(db);
    return ok;
}

bool RoutineStore::remove(const std::string& id) {
    sqlite3* db = open_conn();
    sqlite3_stmt* st = nullptr;
    bool ok = false;
    if (sqlite3_prepare_v2(db, "DELETE FROM routines WHERE id = ?",
                           -1, &st, nullptr) == SQLITE_OK) {
        bind_text(st, 1, id);
        ok = sqlite3_step(st) == SQLITE_DONE && sqlite3_changes(db) > 0;
        sqlite3_finalize(st);
    }
    sqlite3_close(db);
    return ok;
}
