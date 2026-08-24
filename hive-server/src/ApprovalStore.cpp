// =============================================================================
// src/ApprovalStore.cpp — approval queue (HiveMind 2.0, Plan.md Phase 3)
// =============================================================================
#include "hivemind/DbUtil.h"
#include "hivemind/ApprovalStore.h"

#include <array>
#include <chrono>
#include <cstdio>
#include <stdexcept>
#include <thread>

namespace fs = std::filesystem;

namespace {
using hivemind::db::uuid_v4;
using hivemind::db::iso_now;
using hivemind::db::bind_text;
using hivemind::db::col_text;
inline void exec_sql(sqlite3* db, const char* sql) {
    hivemind::db::exec_sql(db, sql, "ApprovalStore");
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
    exec_sql(db,
        "CREATE TABLE IF NOT EXISTS approval_rules ("
        "  agent_id TEXT NOT NULL,"
        "  position INTEGER NOT NULL,"
        "  tool_pattern TEXT NOT NULL,"
        "  command_pattern TEXT NOT NULL,"
        "  action TEXT NOT NULL"
        ")");
    exec_sql(db,
        "CREATE INDEX IF NOT EXISTS idx_approval_rules_agent"
        " ON approval_rules(agent_id)");
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

// ── Phase 16: rule-based auto-review ────────────────────────────────────

bool ApprovalStore::glob_match(std::string_view pat, std::string_view text) {
    // Classic two-pointer glob with '*' backtracking ('?' = one char).
    size_t p = 0, t = 0, star = std::string_view::npos, mark = 0;
    while (t < text.size()) {
        if (p < pat.size() && (pat[p] == '?' || pat[p] == text[t])) {
            ++p; ++t;
        } else if (p < pat.size() && pat[p] == '*') {
            star = p++; mark = t;
        } else if (star != std::string_view::npos) {
            p = star + 1; t = ++mark;
        } else {
            return false;
        }
    }
    while (p < pat.size() && pat[p] == '*') ++p;
    return p == pat.size();
}

std::vector<ApprovalRule> ApprovalStore::list_rules(const std::string& agent_id) {
    std::vector<ApprovalRule> out;
    sqlite3* db = open_conn();
    sqlite3_stmt* st = nullptr;
    const char* sql =
        "SELECT agent_id, position, tool_pattern, command_pattern, action"
        " FROM approval_rules WHERE agent_id = ? ORDER BY position";
    if (sqlite3_prepare_v2(db, sql, -1, &st, nullptr) == SQLITE_OK) {
        bind_text(st, 1, agent_id);
        while (sqlite3_step(st) == SQLITE_ROW) {
            ApprovalRule r;
            r.agent_id = reinterpret_cast<const char*>(sqlite3_column_text(st, 0));
            r.position = sqlite3_column_int(st, 1);
            r.tool_pattern = reinterpret_cast<const char*>(sqlite3_column_text(st, 2));
            r.command_pattern = reinterpret_cast<const char*>(sqlite3_column_text(st, 3));
            r.action = reinterpret_cast<const char*>(sqlite3_column_text(st, 4));
            out.push_back(std::move(r));
        }
    }
    sqlite3_finalize(st);
    sqlite3_close(db);
    return out;
}

void ApprovalStore::replace_rules(const std::string& agent_id,
                                  const std::vector<ApprovalRule>& rules) {
    if (rules.size() > 100)
        throw std::invalid_argument("at most 100 approval rules per agent");
    sqlite3* db = open_conn();
    exec_sql(db, "BEGIN IMMEDIATE");
    try {
        sqlite3_stmt* st = nullptr;
        if (sqlite3_prepare_v2(db,
                "DELETE FROM approval_rules WHERE agent_id = ?",
                -1, &st, nullptr) != SQLITE_OK)
            throw std::runtime_error("rules delete prepare failed");
        bind_text(st, 1, agent_id);
        if (sqlite3_step(st) != SQLITE_DONE) {
            sqlite3_finalize(st);
            throw std::runtime_error("rules delete failed");
        }
        sqlite3_finalize(st);

        st = nullptr;
        if (sqlite3_prepare_v2(db,
                "INSERT INTO approval_rules"
                " (agent_id, position, tool_pattern, command_pattern, action)"
                " VALUES (?,?,?,?,?)",
                -1, &st, nullptr) != SQLITE_OK)
            throw std::runtime_error("rules insert prepare failed");
        int pos = 0;
        for (const auto& r : rules) {
            if (r.action != "allow" && r.action != "deny") {
                sqlite3_finalize(st);
                throw std::invalid_argument("rule action must be allow|deny");
            }
            sqlite3_reset(st);
            bind_text(st, 1, agent_id);
            sqlite3_bind_int(st, 2, pos++);
            bind_text(st, 3, r.tool_pattern);
            bind_text(st, 4, r.command_pattern);
            bind_text(st, 5, r.action);
            if (sqlite3_step(st) != SQLITE_DONE) {
                sqlite3_finalize(st);
                throw std::runtime_error("rules insert failed");
            }
        }
        sqlite3_finalize(st);
        exec_sql(db, "COMMIT");
    } catch (...) {
        exec_sql(db, "ROLLBACK");
        sqlite3_close(db);
        throw;
    }
    sqlite3_close(db);
}

std::optional<bool> ApprovalStore::match_rule(const std::string& agent_id,
                                              const std::string& kind,
                                              const std::string& payload) {
    for (const auto& r : list_rules(agent_id)) {
        if (glob_match(r.tool_pattern, kind) &&
            glob_match(r.command_pattern, payload))
            return r.action == "allow";
    }
    return std::nullopt;
}

void ApprovalStore::record_auto(const std::string& agent_id,
                                const std::string& agent_name,
                                const std::string& kind,
                                const std::string& payload,
                                bool allowed) {
    sqlite3* db = open_conn();
    sqlite3_stmt* st = nullptr;
    const char* sql =
        "INSERT INTO approvals (id, agent_id, agent_name, kind, payload,"
        "                      status, created_at, resolved_at)"
        " VALUES (?,?,?,?,?, ?, ?, ?)";
    if (sqlite3_prepare_v2(db, sql, -1, &st, nullptr) != SQLITE_OK) {
        sqlite3_close(db);
        return;   // best-effort record; never block the decision path
    }
    const std::string id = uuid_v4();
    const std::string now = iso_now();
    bind_text(st, 1, id);
    bind_text(st, 2, agent_id);
    bind_text(st, 3, agent_name);
    bind_text(st, 4, kind);
    bind_text(st, 5, payload);
    bind_text(st, 6, allowed ? "auto_allowed" : "auto_denied");
    bind_text(st, 7, now);
    bind_text(st, 8, now);
    sqlite3_step(st);
    sqlite3_finalize(st);
    sqlite3_close(db);
}
