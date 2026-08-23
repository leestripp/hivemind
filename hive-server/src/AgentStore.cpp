// =============================================================================
// src/AgentStore.cpp — roster persistence (HiveMind 2.0, Plan.md Phase 1)
// =============================================================================
#include "hivemind/AgentStore.h"

#include <array>
#include <chrono>
#include <cstdio>
#include <cstring>
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
        throw std::runtime_error("AgentStore exec failed: " + msg);
    }
}

RosterAgent row_to_agent(sqlite3_stmt* st) {
    RosterAgent a;
    auto col = [&](int i) {
        const auto* p = sqlite3_column_text(st, i);
        return p ? reinterpret_cast<const char*>(p) : "";
    };
    a.id             = col(0);
    a.name           = col(1);
    a.role           = col(2);
    a.model          = col(3);
    a.workspace_mode = col(4);
    a.container_id   = col(5);
    a.created_at     = col(6);
    a.updated_at     = col(7);
    return a;
}

constexpr const char* kSelectCols =
    "SELECT id, name, role, model, workspace_mode, container_id,"
    "       created_at, updated_at FROM agents";

} // namespace

AgentStore::AgentStore() {
    db_path_ = fs::path{std::getenv("HOME") ? std::getenv("HOME") : "."}
             / ".local" / "share" / "hivemind" / "agents.db";
    fs::create_directories(db_path_.parent_path());
    ensure_schema();
}

AgentStore::AgentStore(fs::path db) : db_path_(std::move(db)) {
    fs::create_directories(db_path_.parent_path());
    ensure_schema();
}

AgentStore::~AgentStore() = default;

fs::path AgentStore::agents_base_dir() {
    return fs::path{std::getenv("HOME") ? std::getenv("HOME") : "."}
         / ".local" / "share" / "hivemind" / "agents";
}

sqlite3* AgentStore::open_conn() {
    sqlite3* db = nullptr;
    if (sqlite3_open(db_path_.string().c_str(), &db) != SQLITE_OK) {
        std::string msg = db ? sqlite3_errmsg(db) : "out of memory";
        if (db) sqlite3_close(db);
        throw std::runtime_error("AgentStore open failed: " + msg);
    }
    return db;
}

void AgentStore::ensure_schema() {
    sqlite3* db = open_conn();
    exec_sql(db,
        "CREATE TABLE IF NOT EXISTS agents ("
        "  id TEXT PRIMARY KEY,"
        "  name TEXT NOT NULL UNIQUE,"
        "  role TEXT NOT NULL DEFAULT '',"
        "  model TEXT NOT NULL DEFAULT '',"
        "  workspace_mode TEXT NOT NULL DEFAULT 'shared',"
        "  container_id TEXT NOT NULL DEFAULT '',"
        "  created_at TEXT NOT NULL,"
        "  updated_at TEXT NOT NULL"
        ")");
    sqlite3_close(db);
}

RosterAgent AgentStore::create_agent(const std::string& name,
                                     const std::string& role,
                                     const std::string& model,
                                     const std::string& workspace_mode) {
    if (name.empty())
        throw std::invalid_argument("agent name must not be empty");
    if (get_agent_by_name(name))
        throw std::invalid_argument("an agent named '" + name + "' already exists");

    RosterAgent a;
    a.id             = uuid_v4();
    a.name           = name;
    a.role           = role;
    a.model          = model;
    a.workspace_mode = workspace_mode.empty() ? "shared" : workspace_mode;
    a.created_at     = iso_now();
    a.updated_at     = a.created_at;

    sqlite3* db = open_conn();
    sqlite3_stmt* st = nullptr;
    const char* sql =
        "INSERT INTO agents (id, name, role, model, workspace_mode,"
        "                   container_id, created_at, updated_at)"
        " VALUES (?,?,?,?,?,'',?,?)";
    if (sqlite3_prepare_v2(db, sql, -1, &st, nullptr) != SQLITE_OK) {
        std::string msg = sqlite3_errmsg(db);
        sqlite3_close(db);
        throw std::runtime_error("AgentStore insert prepare failed: " + msg);
    }
    bind_text(st, 1, a.id);
    bind_text(st, 2, a.name);
    bind_text(st, 3, a.role);
    bind_text(st, 4, a.model);
    bind_text(st, 5, a.workspace_mode);
    bind_text(st, 6, a.created_at);
    bind_text(st, 7, a.updated_at);
    bool ok = sqlite3_step(st) == SQLITE_DONE;
    sqlite3_finalize(st);
    sqlite3_close(db);
    if (!ok) throw std::runtime_error("AgentStore insert failed");
    return a;
}

std::optional<RosterAgent> AgentStore::get_agent(const std::string& id) {
    sqlite3* db = open_conn();
    sqlite3_stmt* st = nullptr;
    std::string sql = std::string{kSelectCols} + " WHERE id = ?";
    std::optional<RosterAgent> out;
    if (sqlite3_prepare_v2(db, sql.c_str(), -1, &st, nullptr) == SQLITE_OK) {
        bind_text(st, 1, id);
        if (sqlite3_step(st) == SQLITE_ROW) out = row_to_agent(st);
        sqlite3_finalize(st);
    }
    sqlite3_close(db);
    return out;
}

std::optional<RosterAgent> AgentStore::get_agent_by_name(const std::string& name) {
    sqlite3* db = open_conn();
    sqlite3_stmt* st = nullptr;
    std::string sql = std::string{kSelectCols} + " WHERE name = ?";
    std::optional<RosterAgent> out;
    if (sqlite3_prepare_v2(db, sql.c_str(), -1, &st, nullptr) == SQLITE_OK) {
        bind_text(st, 1, name);
        if (sqlite3_step(st) == SQLITE_ROW) out = row_to_agent(st);
        sqlite3_finalize(st);
    }
    sqlite3_close(db);
    return out;
}

std::vector<RosterAgent> AgentStore::list_agents() {
    sqlite3* db = open_conn();
    sqlite3_stmt* st = nullptr;
    std::string sql = std::string{kSelectCols} + " ORDER BY created_at ASC";
    std::vector<RosterAgent> out;
    if (sqlite3_prepare_v2(db, sql.c_str(), -1, &st, nullptr) == SQLITE_OK) {
        while (sqlite3_step(st) == SQLITE_ROW) out.push_back(row_to_agent(st));
        sqlite3_finalize(st);
    }
    sqlite3_close(db);
    return out;
}

RosterAgent AgentStore::update_agent(const std::string& id,
                                     const std::string& name,
                                     const std::string& role,
                                     const std::string& model,
                                     const std::string& workspace_mode) {
    auto existing = get_agent(id);
    if (!existing)
        throw std::invalid_argument("agent not found: " + id);
    if (name.empty())
        throw std::invalid_argument("agent name must not be empty");
    if (auto clash = get_agent_by_name(name); clash && clash->id != id)
        throw std::invalid_argument("an agent named '" + name + "' already exists");

    std::string now = iso_now();
    sqlite3* db = open_conn();
    sqlite3_stmt* st = nullptr;
    const char* sql =
        "UPDATE agents SET name=?, role=?, model=?, workspace_mode=?, updated_at=?"
        " WHERE id=?";
    if (sqlite3_prepare_v2(db, sql, -1, &st, nullptr) != SQLITE_OK) {
        std::string msg = sqlite3_errmsg(db);
        sqlite3_close(db);
        throw std::runtime_error("AgentStore update prepare failed: " + msg);
    }
    bind_text(st, 1, name);
    bind_text(st, 2, role);
    bind_text(st, 3, model);
    bind_text(st, 4, workspace_mode.empty() ? "shared" : workspace_mode);
    bind_text(st, 5, now);
    bind_text(st, 6, id);
    bool ok = sqlite3_step(st) == SQLITE_DONE;
    sqlite3_finalize(st);
    sqlite3_close(db);
    if (!ok) throw std::runtime_error("AgentStore update failed");

    auto updated = get_agent(id);
    return *updated;
}

bool AgentStore::delete_agent(const std::string& id) {
    sqlite3* db = open_conn();
    sqlite3_stmt* st = nullptr;
    const char* sql = "DELETE FROM agents WHERE id = ?";
    bool ok = false;
    if (sqlite3_prepare_v2(db, sql, -1, &st, nullptr) == SQLITE_OK) {
        bind_text(st, 1, id);
        ok = sqlite3_step(st) == SQLITE_DONE && sqlite3_changes(db) > 0;
        sqlite3_finalize(st);
    }
    sqlite3_close(db);
    return ok;
}
