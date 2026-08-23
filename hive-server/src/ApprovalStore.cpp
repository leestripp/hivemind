// =============================================================================
// src/ApprovalStore.cpp — approval queue (HiveMind 2.0, Plan.md Phase 3)
// =============================================================================
#include "hivemind/ApprovalStore.h"

#include <array>
#include <chrono>
#include <cstdio>
#include <stdexcept>
#include <thread>

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
        throw std::runtime_error("ApprovalStore exec failed: " + msg);
    }
}

const char* col_text(sqlite3_stmt* st, int i) {
    const auto* p = sqlite3_column_text(st, i);
    return p ? reinterpret_cast<const char*>(p) : "";
}

Approval row_to_approval(sqlite3_stmt* st) {
    Approval a;
    a.id          = col_text(st, 0);
    a.agent_id    = col_text(st, 1);
    a.agent_name  = col_text(st, 2);
    a.kind        = col_text(st, 3);
    a.payload     = col_text(st, 4);
    a.status      = col_text(st, 5);
    a.created_at  = col_text(st, 6);
    a.resolved_at = col_text(st, 7);
    return a;
}

constexpr const char* kCols =
    "SELECT id, agent_id, agent_name, kind, payload, status,"
    "       created_at, resolved_at FROM approvals";

} // namespace

ApprovalStore::ApprovalStore() {
    db_path_ = fs::path{std::getenv("HOME") ? std::getenv("HOME") : "."}
             / ".local" / "share" / "hivemind" / "approvals.db";
    fs::create_directories(db_path_.parent_path());
    ensure_schema();
}

ApprovalStore::ApprovalStore(fs::path db) : db_path_(std::move(db)) {
    fs::create_directories(db_path_.parent_path());
    ensure_schema();
}

ApprovalStore::~ApprovalStore() = default;

sqlite3* ApprovalStore::open_conn() {
    sqlite3* db = nullptr;
    if (sqlite3_open(db_path_.string().c_str(), &db) != SQLITE_OK) {
        std::string msg = db ? sqlite3_errmsg(db) : "out of memory";
        if (db) sqlite3_close(db);
        throw std::runtime_error("ApprovalStore open failed: " + msg);
    }
    return db;
}

void ApprovalStore::ensure_schema() {
    sqlite3* db = open_conn();
    exec_sql(db,
        "CREATE TABLE IF NOT EXISTS approvals ("
        "  id TEXT PRIMARY KEY,"
        "  agent_id TEXT NOT NULL,"
        "  agent_name TEXT NOT NULL DEFAULT '',"
        "  kind TEXT NOT NULL DEFAULT 'command',"
        "  payload TEXT NOT NULL DEFAULT '',"
        "  status TEXT NOT NULL DEFAULT 'pending',"
        "  created_at TEXT NOT NULL,"
        "  resolved_at TEXT NOT NULL DEFAULT ''"
        ")");
    exec_sql(db,
        "CREATE INDEX IF NOT EXISTS idx_approvals_status ON approvals(status)");
    sqlite3_close(db);
}

std::string ApprovalStore::create(const std::string& agent_id,
                                  const std::string& agent_name,
                                  const std::string& kind,
                                  const std::string& payload) {
    std::string id = uuid_v4();
    sqlite3* db = open_conn();
    sqlite3_stmt* st = nullptr;
    const char* sql =
        "INSERT INTO approvals (id, agent_id, agent_name, kind, payload,"
        "                      status, created_at, resolved_at)"
        " VALUES (?,?,?,?,?, 'pending', ?, '')";
    if (sqlite3_prepare_v2(db, sql, -1, &st, nullptr) != SQLITE_OK) {
        std::string msg = sqlite3_errmsg(db);
        sqlite3_close(db);
        throw std::runtime_error("ApprovalStore insert prepare failed: " + msg);
    }
    bind_text(st, 1, id);
    bind_text(st, 2, agent_id);
    bind_text(st, 3, agent_name);
    bind_text(st, 4, kind);
    bind_text(st, 5, payload);
    bind_text(st, 6, iso_now());
    bool ok = sqlite3_step(st) == SQLITE_DONE;
    sqlite3_finalize(st);
    sqlite3_close(db);
    if (!ok) throw std::runtime_error("ApprovalStore insert failed");
    return id;
}

std::optional<Approval> ApprovalStore::get(const std::string& id) {
    sqlite3* db = open_conn();
    std::optional<Approval> out;
    sqlite3_stmt* st = nullptr;
    std::string sql = std::string{kCols} + " WHERE id = ?";
    if (sqlite3_prepare_v2(db, sql.c_str(), -1, &st, nullptr) == SQLITE_OK) {
        bind_text(st, 1, id);
        if (sqlite3_step(st) == SQLITE_ROW) out = row_to_approval(st);
        sqlite3_finalize(st);
    }
    sqlite3_close(db);
    return out;
}

std::vector<Approval> ApprovalStore::list(const std::string& status) {
    sqlite3* db = open_conn();
    std::vector<Approval> out;
    sqlite3_stmt* st = nullptr;
    std::string sql = std::string{kCols} +
        (status.empty() ? " ORDER BY created_at DESC"
                        : " WHERE status = ? ORDER BY created_at DESC");
    if (sqlite3_prepare_v2(db, sql.c_str(), -1, &st, nullptr) == SQLITE_OK) {
        if (!status.empty()) bind_text(st, 1, status);
        while (sqlite3_step(st) == SQLITE_ROW) out.push_back(row_to_approval(st));
        sqlite3_finalize(st);
    }
    sqlite3_close(db);
    return out;
}

bool ApprovalStore::resolve(const std::string& id, bool approved) {
    sqlite3* db = open_conn();
    sqlite3_stmt* st = nullptr;
    const char* sql =
        "UPDATE approvals SET status = ?, resolved_at = ?"
        " WHERE id = ? AND status = 'pending'";
    bool ok = false;
    if (sqlite3_prepare_v2(db, sql, -1, &st, nullptr) == SQLITE_OK) {
        bind_text(st, 1, approved ? "approved" : "denied");
        bind_text(st, 2, iso_now());
        bind_text(st, 3, id);
        ok = sqlite3_step(st) == SQLITE_DONE && sqlite3_changes(db) > 0;
        sqlite3_finalize(st);
    }
    sqlite3_close(db);
    return ok;
}

bool ApprovalStore::wait(const std::string& id, int timeout_s) {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeout_s);
    while (std::chrono::steady_clock::now() < deadline) {
        auto a = get(id);
        if (!a) return false;
        if (a->status == "approved") return true;
        if (a->status != "pending") return false;  // denied / expired
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
    // Timed out — mark expired so the UI shows it didn't just vanish.
    sqlite3* db = open_conn();
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db,
            "UPDATE approvals SET status = 'expired', resolved_at = ?"
            " WHERE id = ? AND status = 'pending'",
            -1, &st, nullptr) == SQLITE_OK) {
        bind_text(st, 1, iso_now());
        bind_text(st, 2, id);
        sqlite3_step(st);
        sqlite3_finalize(st);
    }
    sqlite3_close(db);
    return false;
}
