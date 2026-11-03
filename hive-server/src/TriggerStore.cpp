#include "hivemind/TriggerStore.h"

#include "hivemind/DbUtil.h"

#include <cstdlib>
#include <stdexcept>
#include <utility>

namespace fs = std::filesystem;

namespace {
using hivemind::db::bind_text;
using hivemind::db::col_text;
using hivemind::db::iso_now;
using hivemind::db::uuid_v4;

void exec_sql(sqlite3* db, const char* sql) {
    hivemind::db::exec_sql(db, sql, "TriggerStore");
}

EventTrigger row_to_trigger(sqlite3_stmt* st) {
    EventTrigger trigger;
    trigger.id = col_text(st, 0);
    trigger.agent_id = col_text(st, 1);
    trigger.thread_id = col_text(st, 2);
    trigger.event_type = col_text(st, 3);
    trigger.target_agent_id = col_text(st, 4);
    trigger.prompt = col_text(st, 5);
    trigger.created_at = col_text(st, 6);
    trigger.claimed = sqlite3_column_int(st, 7) != 0;
    return trigger;
}

constexpr const char* kCols =
    "SELECT id, agent_id, thread_id, event_type, target_agent_id, prompt,"
    "       created_at, claimed FROM event_triggers";
} // namespace

nlohmann::json trigger_json(const EventTrigger& trigger) {
    return {
        {"id", trigger.id},
        {"agent_id", trigger.agent_id},
        {"thread_id", trigger.thread_id},
        {"event", trigger.event_type},
        {"target_agent_id", trigger.target_agent_id},
        {"prompt", trigger.prompt},
        {"created_at", trigger.created_at},
        {"state", trigger.claimed ? "firing" : "pending"},
    };
}

TriggerStore::TriggerStore() {
    db_path_ = fs::path{std::getenv("HOME") ? std::getenv("HOME") : "."}
             / ".local" / "share" / "hivemind" / "triggers.db";
    fs::create_directories(db_path_.parent_path());
    ensure_schema();
}

TriggerStore::TriggerStore(fs::path db) : db_path_(std::move(db)) {
    fs::create_directories(db_path_.parent_path());
    ensure_schema();
}

TriggerStore::~TriggerStore() = default;

sqlite3* TriggerStore::open_conn() const {
    sqlite3* db = nullptr;
    if (sqlite3_open(db_path_.string().c_str(), &db) != SQLITE_OK) {
        const std::string msg = db ? sqlite3_errmsg(db) : "out of memory";
        if (db) sqlite3_close(db);
        throw std::runtime_error("TriggerStore open failed: " + msg);
    }
    sqlite3_busy_timeout(db, 5000);
    return db;
}

void TriggerStore::ensure_schema() {
    sqlite3* db = open_conn();
    try {
        exec_sql(db,
            "CREATE TABLE IF NOT EXISTS event_triggers ("
            "  id TEXT PRIMARY KEY,"
            "  agent_id TEXT NOT NULL,"
            "  thread_id TEXT NOT NULL,"
            "  event_type TEXT NOT NULL,"
            "  target_agent_id TEXT NOT NULL,"
            "  prompt TEXT NOT NULL DEFAULT '',"
            "  created_at TEXT NOT NULL,"
            "  claimed INTEGER NOT NULL DEFAULT 0"
            ")");
        exec_sql(db,
            "CREATE TABLE IF NOT EXISTS agent_background ("
            "  agent_id TEXT PRIMARY KEY,"
            "  active_count INTEGER NOT NULL CHECK(active_count > 0),"
            "  updated_at TEXT NOT NULL"
            ")");
        exec_sql(db,
            "CREATE INDEX IF NOT EXISTS idx_event_triggers_event"
            " ON event_triggers(event_type, target_agent_id, claimed)");
        exec_sql(db,
            "CREATE INDEX IF NOT EXISTS idx_event_triggers_owner"
            " ON event_triggers(agent_id, created_at)");
        // A hub stop between claim and completion must not lose the event.
        // Re-delivery after a crash is possible; silent loss is not.
        exec_sql(db, "UPDATE event_triggers SET claimed = 0 WHERE claimed = 1");
    } catch (...) {
        sqlite3_close(db);
        throw;
    }
    sqlite3_close(db);
}

EventTrigger TriggerStore::create(const std::string& agent_id,
                                  const std::string& thread_id,
                                  const std::string& event_type,
                                  const std::string& target_agent_id,
                                  const std::string& prompt) {
    EventTrigger trigger;
    trigger.id = uuid_v4();
    trigger.agent_id = agent_id;
    trigger.thread_id = thread_id;
    trigger.event_type = event_type;
    trigger.target_agent_id = target_agent_id;
    trigger.prompt = prompt;
    trigger.created_at = iso_now();

    sqlite3* db = open_conn();
    sqlite3_stmt* st = nullptr;
    const char* sql =
        "INSERT INTO event_triggers (id, agent_id, thread_id, event_type,"
        " target_agent_id, prompt, created_at, claimed)"
        " VALUES (?,?,?,?,?,?,?,0)";
    if (sqlite3_prepare_v2(db, sql, -1, &st, nullptr) != SQLITE_OK) {
        const std::string msg = sqlite3_errmsg(db);
        sqlite3_close(db);
        throw std::runtime_error("TriggerStore insert prepare failed: " + msg);
    }
    bind_text(st, 1, trigger.id);
    bind_text(st, 2, trigger.agent_id);
    bind_text(st, 3, trigger.thread_id);
    bind_text(st, 4, trigger.event_type);
    bind_text(st, 5, trigger.target_agent_id);
    bind_text(st, 6, trigger.prompt);
    bind_text(st, 7, trigger.created_at);
    const bool ok = sqlite3_step(st) == SQLITE_DONE;
    sqlite3_finalize(st);
    sqlite3_close(db);
    if (!ok) throw std::runtime_error("TriggerStore insert failed");
    return trigger;
}

std::optional<EventTrigger> TriggerStore::get(const std::string& id) {
    sqlite3* db = open_conn();
    std::optional<EventTrigger> out;
    sqlite3_stmt* st = nullptr;
    const std::string sql = std::string{kCols} + " WHERE id = ?";
    if (sqlite3_prepare_v2(db, sql.c_str(), -1, &st, nullptr) == SQLITE_OK) {
        bind_text(st, 1, id);
        if (sqlite3_step(st) == SQLITE_ROW) out = row_to_trigger(st);
        sqlite3_finalize(st);
    }
    sqlite3_close(db);
    return out;
}

std::vector<EventTrigger> TriggerStore::list(const std::string& agent_id) {
    sqlite3* db = open_conn();
    std::vector<EventTrigger> out;
    sqlite3_stmt* st = nullptr;
    const std::string sql = std::string{kCols}
        + (agent_id.empty() ? "" : " WHERE agent_id = ?")
        + " ORDER BY created_at DESC, id DESC";
    if (sqlite3_prepare_v2(db, sql.c_str(), -1, &st, nullptr) == SQLITE_OK) {
        if (!agent_id.empty()) bind_text(st, 1, agent_id);
        while (sqlite3_step(st) == SQLITE_ROW)
            out.push_back(row_to_trigger(st));
        sqlite3_finalize(st);
    }
    sqlite3_close(db);
    return out;
}

std::vector<EventTrigger> TriggerStore::claim(
        const std::string& event_type,
        const std::string& target_agent_id) {
    sqlite3* db = open_conn();
    std::vector<EventTrigger> out;
    try {
        exec_sql(db, "BEGIN IMMEDIATE");
        sqlite3_stmt* st = nullptr;
        const std::string select = std::string{kCols}
            + " WHERE event_type = ? AND target_agent_id = ? AND claimed = 0"
              " ORDER BY created_at, id";
        if (sqlite3_prepare_v2(db, select.c_str(), -1, &st, nullptr)
                != SQLITE_OK)
            throw std::runtime_error("TriggerStore claim select failed");
        bind_text(st, 1, event_type);
        bind_text(st, 2, target_agent_id);
        while (sqlite3_step(st) == SQLITE_ROW)
            out.push_back(row_to_trigger(st));
        sqlite3_finalize(st);

        st = nullptr;
        const char* update =
            "UPDATE event_triggers SET claimed = 1"
            " WHERE event_type = ? AND target_agent_id = ? AND claimed = 0";
        if (sqlite3_prepare_v2(db, update, -1, &st, nullptr) != SQLITE_OK)
            throw std::runtime_error("TriggerStore claim update failed");
        bind_text(st, 1, event_type);
        bind_text(st, 2, target_agent_id);
        if (sqlite3_step(st) != SQLITE_DONE) {
            sqlite3_finalize(st);
            throw std::runtime_error("TriggerStore claim update failed");
        }
        sqlite3_finalize(st);
        exec_sql(db, "COMMIT");
    } catch (...) {
        sqlite3_exec(db, "ROLLBACK", nullptr, nullptr, nullptr);
        sqlite3_close(db);
        throw;
    }
    for (auto& trigger : out) trigger.claimed = true;
    sqlite3_close(db);
    return out;
}

bool TriggerStore::complete(const std::string& id) {
    sqlite3* db = open_conn();
    sqlite3_stmt* st = nullptr;
    bool ok = sqlite3_prepare_v2(db,
        "DELETE FROM event_triggers WHERE id = ? AND claimed = 1",
        -1, &st, nullptr) == SQLITE_OK;
    if (ok) {
        bind_text(st, 1, id);
        ok = sqlite3_step(st) == SQLITE_DONE && sqlite3_changes(db) > 0;
    }
    sqlite3_finalize(st);
    sqlite3_close(db);
    return ok;
}

bool TriggerStore::release(const std::string& id) {
    sqlite3* db = open_conn();
    sqlite3_stmt* st = nullptr;
    bool ok = sqlite3_prepare_v2(db,
        "UPDATE event_triggers SET claimed = 0 WHERE id = ? AND claimed = 1",
        -1, &st, nullptr) == SQLITE_OK;
    if (ok) {
        bind_text(st, 1, id);
        ok = sqlite3_step(st) == SQLITE_DONE && sqlite3_changes(db) > 0;
    }
    sqlite3_finalize(st);
    sqlite3_close(db);
    return ok;
}

bool TriggerStore::remove(const std::string& id) {
    sqlite3* db = open_conn();
    sqlite3_stmt* st = nullptr;
    bool ok = sqlite3_prepare_v2(db,
        "DELETE FROM event_triggers WHERE id = ?", -1, &st, nullptr)
        == SQLITE_OK;
    if (ok) {
        bind_text(st, 1, id);
        ok = sqlite3_step(st) == SQLITE_DONE && sqlite3_changes(db) > 0;
    }
    sqlite3_finalize(st);
    sqlite3_close(db);
    return ok;
}

int TriggerStore::remove_for_agent(const std::string& agent_id) {
    sqlite3* db = open_conn();
    sqlite3_stmt* st = nullptr;
    int count = 0;
    if (sqlite3_prepare_v2(db,
            "DELETE FROM event_triggers WHERE agent_id = ?"
            " OR target_agent_id = ?", -1, &st, nullptr) == SQLITE_OK) {
        bind_text(st, 1, agent_id);
        bind_text(st, 2, agent_id);
        if (sqlite3_step(st) == SQLITE_DONE) count = sqlite3_changes(db);
        sqlite3_finalize(st);
    }
    sqlite3_close(db);
    set_background_active(agent_id, 0);
    return count;
}

void TriggerStore::set_background_active(const std::string& agent_id,
                                         int active_count) {
    if (active_count < 0)
        throw std::invalid_argument("background active count cannot be negative");
    sqlite3* db = open_conn();
    sqlite3_stmt* st = nullptr;
    const char* sql = active_count == 0
        ? "DELETE FROM agent_background WHERE agent_id = ?"
        : "INSERT INTO agent_background(agent_id, active_count, updated_at)"
          " VALUES(?, ?, ?) ON CONFLICT(agent_id) DO UPDATE SET"
          " active_count = excluded.active_count, updated_at = excluded.updated_at";
    const bool prepared = sqlite3_prepare_v2(db, sql, -1, &st, nullptr)
        == SQLITE_OK;
    if (!prepared) {
        const std::string msg = sqlite3_errmsg(db);
        sqlite3_close(db);
        throw std::runtime_error("TriggerStore background prepare failed: " + msg);
    }
    bind_text(st, 1, agent_id);
    if (active_count > 0) {
        sqlite3_bind_int(st, 2, active_count);
        bind_text(st, 3, iso_now());
    }
    const bool ok = sqlite3_step(st) == SQLITE_DONE;
    sqlite3_finalize(st);
    if (!ok) {
        const std::string msg = sqlite3_errmsg(db);
        sqlite3_close(db);
        throw std::runtime_error("TriggerStore background update failed: " + msg);
    }
    sqlite3_close(db);
}

int TriggerStore::background_active(const std::string& agent_id) const {
    sqlite3* db = open_conn();
    sqlite3_stmt* st = nullptr;
    int count = 0;
    if (sqlite3_prepare_v2(db,
            "SELECT active_count FROM agent_background WHERE agent_id = ?",
            -1, &st, nullptr) != SQLITE_OK) {
        const std::string msg = sqlite3_errmsg(db);
        sqlite3_close(db);
        throw std::runtime_error("TriggerStore background read failed: " + msg);
    }
    bind_text(st, 1, agent_id);
    const int rc = sqlite3_step(st);
    if (rc == SQLITE_ROW) count = sqlite3_column_int(st, 0);
    else if (rc != SQLITE_DONE) {
        const std::string msg = sqlite3_errmsg(db);
        sqlite3_finalize(st);
        sqlite3_close(db);
        throw std::runtime_error("TriggerStore background read failed: " + msg);
    }
    sqlite3_finalize(st);
    sqlite3_close(db);
    return count;
}
