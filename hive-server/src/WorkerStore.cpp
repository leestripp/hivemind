// src/WorkerStore.cpp — Phase 30.1: hive-worker registry.
// See WorkerStore.h for the model. Follows the ConnectorStore pattern:
// fresh SQLite connection per op, DbUtil helpers, hashed keys.

#include "hivemind/WorkerStore.h"
#include "hivemind/DbUtil.h"
#include "token/TokenUtils.h"   // OmniNet: generateApiKey, hashToken

#include <stdexcept>

namespace fs = std::filesystem;
using hivemind::db::bind_text;
using hivemind::db::col_text;
using hivemind::db::iso_now;

namespace {

WorkerInfo row_to_worker(sqlite3_stmt* st) {
    WorkerInfo w;
    w.name       = col_text(st, 0);
    w.caps       = col_text(st, 1);
    w.proto      = sqlite3_column_int(st, 2);
    w.last_seen  = col_text(st, 3);
    w.created_at = col_text(st, 4);
    return w;
}

constexpr const char* kSelectCols =
    "SELECT name, caps, proto, last_seen, created_at FROM workers";

} // namespace

WorkerStore::WorkerStore() {
    db_path_ = fs::path{std::getenv("HOME") ? std::getenv("HOME") : "."}
             / ".local" / "share" / "hivemind" / "workers.db";
    ensure_schema();
}

WorkerStore::WorkerStore(fs::path db) : db_path_(std::move(db)) {
    ensure_schema();
}

WorkerStore::~WorkerStore() = default;

void WorkerStore::ensure_schema() {
    std::error_code ec;
    fs::create_directories(db_path_.parent_path(), ec);
    sqlite3* db = open_conn();
    sqlite3_exec(db,
        "CREATE TABLE IF NOT EXISTS workers ("
        "  name TEXT PRIMARY KEY,"
        "  key_hash TEXT NOT NULL,"
        "  caps TEXT NOT NULL DEFAULT '{}',"
        "  proto INTEGER NOT NULL DEFAULT 1,"
        "  last_seen TEXT NOT NULL DEFAULT '',"
        "  created_at TEXT NOT NULL"
        ")",
        nullptr, nullptr, nullptr);
    sqlite3_close(db);
}

sqlite3* WorkerStore::open_conn() {
    sqlite3* db = nullptr;
    if (sqlite3_open(db_path_.c_str(), &db) != SQLITE_OK) {
        std::string msg = db ? sqlite3_errmsg(db) : "oom";
        if (db) sqlite3_close(db);
        throw std::runtime_error("WorkerStore open failed: " + msg);
    }
    // Wait up to 5s on a contended write lock instead of
    // failing instantly (concurrent POST bursts otherwise
    // 500 with "database is locked").
    sqlite3_busy_timeout(db, 5000);
    return db;
}

bool WorkerStore::valid_name(const std::string& n) {
    if (n.empty() || n.size() > 32) return false;
    for (char c : n)
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-'))
            return false;
    return true;
}

std::string WorkerStore::enroll(const std::string& name) {
    if (!valid_name(name))
        throw std::invalid_argument("worker name must be [a-z0-9-], 1-32 chars");
    if (get(name))
        throw std::invalid_argument("worker '" + name + "' already enrolled");

    std::string raw = OmniNet::Token::generateApiKey("hmwk");
    std::string hash = OmniNet::Token::hashToken(raw);

    sqlite3* db = open_conn();
    sqlite3_stmt* st = nullptr;
    const char* sql =
        "INSERT INTO workers (name, key_hash, created_at) VALUES (?,?,?)";
    if (sqlite3_prepare_v2(db, sql, -1, &st, nullptr) != SQLITE_OK) {
        std::string msg = sqlite3_errmsg(db);
        sqlite3_close(db);
        throw std::runtime_error("WorkerStore enroll prepare failed: " + msg);
    }
    bind_text(st, 1, name);
    bind_text(st, 2, hash);
    bind_text(st, 3, iso_now());
    bool ok = sqlite3_step(st) == SQLITE_DONE;
    sqlite3_finalize(st);
    sqlite3_close(db);
    if (!ok) throw std::runtime_error("WorkerStore enroll failed");
    return raw;   // shown once by the CLI; only the hash persists
}

std::optional<WorkerInfo> WorkerStore::validate(const std::string& raw_key,
                                                const std::string& caps,
                                                int proto) {
    if (raw_key.empty()) return std::nullopt;
    std::string hash = OmniNet::Token::hashToken(raw_key);
    std::string now = iso_now();

    sqlite3* db = open_conn();
    sqlite3_stmt* st = nullptr;
    std::optional<WorkerInfo> out;
    const char* find =
        "SELECT name FROM workers WHERE key_hash = ?";
    std::string name;
    if (sqlite3_prepare_v2(db, find, -1, &st, nullptr) == SQLITE_OK) {
        bind_text(st, 1, hash);
        if (sqlite3_step(st) == SQLITE_ROW) name = col_text(st, 0);
        sqlite3_finalize(st);
    }
    if (!name.empty()) {
        // Refresh liveness (+ hello payload) in the same shot. caps only
        // updates when the caller actually has a fresh hello payload —
        // heartbeats call validate(key) with no caps and must NOT clobber
        // what the hello stored (regression: every beat wiped caps to {}).
        sqlite3_stmt* up = nullptr;
        const char* sql =
            "UPDATE workers SET last_seen=?,"
            " caps = CASE WHEN ? = '' OR ? = '{}' THEN caps ELSE ? END,"
            " proto=? WHERE name=?";
        if (sqlite3_prepare_v2(db, sql, -1, &up, nullptr) == SQLITE_OK) {
            bind_text(up, 1, now);
            bind_text(up, 2, caps);
            bind_text(up, 3, caps);
            bind_text(up, 4, caps.empty() ? "{}" : caps);
            bind_text(up, 5, std::to_string(proto));
            bind_text(up, 6, name);
            sqlite3_step(up);
            sqlite3_finalize(up);
        }
        sqlite3_close(db);
        out = get(name);   // fresh row
        return out;
    }
    sqlite3_close(db);
    return std::nullopt;
}

std::optional<WorkerInfo> WorkerStore::get(const std::string& name) {
    sqlite3* db = open_conn();
    sqlite3_stmt* st = nullptr;
    std::string sql = std::string{kSelectCols} + " WHERE name = ?";
    std::optional<WorkerInfo> out;
    if (sqlite3_prepare_v2(db, sql.c_str(), -1, &st, nullptr) == SQLITE_OK) {
        bind_text(st, 1, name);
        if (sqlite3_step(st) == SQLITE_ROW) out = row_to_worker(st);
        sqlite3_finalize(st);
    }
    sqlite3_close(db);
    return out;
}

std::vector<WorkerInfo> WorkerStore::list() {
    sqlite3* db = open_conn();
    sqlite3_stmt* st = nullptr;
    std::vector<WorkerInfo> out;
    std::string sql = std::string{kSelectCols} + " ORDER BY name";
    if (sqlite3_prepare_v2(db, sql.c_str(), -1, &st, nullptr) == SQLITE_OK) {
        while (sqlite3_step(st) == SQLITE_ROW) out.push_back(row_to_worker(st));
        sqlite3_finalize(st);
    }
    sqlite3_close(db);
    return out;
}

bool WorkerStore::remove(const std::string& name) {
    sqlite3* db = open_conn();
    sqlite3_stmt* st = nullptr;
    bool ok = false;
    if (sqlite3_prepare_v2(db, "DELETE FROM workers WHERE name = ?",
                           -1, &st, nullptr) == SQLITE_OK) {
        bind_text(st, 1, name);
        ok = sqlite3_step(st) == SQLITE_DONE && sqlite3_changes(db) > 0;
        sqlite3_finalize(st);
    }
    sqlite3_close(db);
    return ok;
}
