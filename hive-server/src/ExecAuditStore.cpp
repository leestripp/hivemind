// src/ExecAuditStore.cpp — per-agent container exec audit trail.
#include "hivemind/ExecAuditStore.h"
#include "hivemind/DbUtil.h"

#include <sqlite3.h>

using namespace hivemind::db;

ExecAuditStore::ExecAuditStore(std::filesystem::path db_path)
    : db_path_(std::move(db_path)) {
    std::error_code ec;
    std::filesystem::create_directories(db_path_.parent_path(), ec);
    sqlite3* db = open_conn();
    exec_sql(db,
        "CREATE TABLE IF NOT EXISTS exec_audit ("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  ts TEXT NOT NULL,"
        "  agent_id TEXT NOT NULL,"
        "  command TEXT NOT NULL DEFAULT '',"
        "  exit_code INTEGER NOT NULL DEFAULT 0,"
        "  timed_out INTEGER NOT NULL DEFAULT 0,"
        "  duration_ms INTEGER NOT NULL DEFAULT 0"
        ")",
        "exec_audit table");
    exec_sql(db,
        "CREATE INDEX IF NOT EXISTS idx_exec_audit_agent"
        " ON exec_audit(agent_id, id DESC)",
        "idx_exec_audit_agent");
    sqlite3_close(db);
}

sqlite3* ExecAuditStore::open_conn() {
    sqlite3* db = nullptr;
    if (sqlite3_open(db_path_.c_str(), &db) != SQLITE_OK)
        throw std::runtime_error("cannot open exec audit db: " +
                                 db_path_.string());
    return db;
}

void ExecAuditStore::log(const std::string& agent_id,
                         const std::string& command, int exit_code,
                         bool timed_out, int64_t duration_ms) {
    sqlite3* db = open_conn();
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db,
            "INSERT INTO exec_audit"
            " (ts, agent_id, command, exit_code, timed_out, duration_ms)"
            " VALUES (?,?,?,?,?,?)", -1, &st, nullptr) == SQLITE_OK) {
        const std::string ts = iso_now();
        std::string cmd = command.size() > kCommandLimit
            ? command.substr(0, kCommandLimit) + "…" : command;
        bind_text(st, 1, ts);
        bind_text(st, 2, agent_id);
        bind_text(st, 3, cmd);
        sqlite3_bind_int(st, 4, exit_code);
        sqlite3_bind_int(st, 5, timed_out ? 1 : 0);
        sqlite3_bind_int64(st, 6, duration_ms);
        (void)sqlite3_step(st);
        sqlite3_finalize(st);
    }
    sqlite3_close(db);
}

std::vector<ExecAuditStore::Entry> ExecAuditStore::list(
    const std::string& agent_id, int limit) {
    sqlite3* db = open_conn();
    std::vector<Entry> out;
    sqlite3_stmt* st = nullptr;
    const std::string sql =
        agent_id.empty()
            ? "SELECT id, ts, agent_id, command, exit_code, timed_out,"
              " duration_ms FROM exec_audit ORDER BY id DESC LIMIT ?"
            : "SELECT id, ts, agent_id, command, exit_code, timed_out,"
              " duration_ms FROM exec_audit WHERE agent_id = ?"
              " ORDER BY id DESC LIMIT ?";
    if (sqlite3_prepare_v2(db, sql.c_str(), -1, &st, nullptr) == SQLITE_OK) {
        int bind = 1;
        if (!agent_id.empty()) bind_text(st, bind++, agent_id);
        sqlite3_bind_int(st, bind, limit);
        while (sqlite3_step(st) == SQLITE_ROW) {
            Entry e;
            e.id = sqlite3_column_int64(st, 0);
            e.ts = col_text(st, 1);
            e.agent_id = col_text(st, 2);
            e.command = col_text(st, 3);
            e.exit_code = sqlite3_column_int(st, 4);
            e.timed_out = sqlite3_column_int(st, 5) != 0;
            e.duration_ms = sqlite3_column_int64(st, 6);
            out.push_back(std::move(e));
        }
        sqlite3_finalize(st);
    }
    sqlite3_close(db);
    return out;
}
