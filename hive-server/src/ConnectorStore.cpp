// src/ConnectorStore.cpp — Phase 23: OAuth2 connector registrations.
#include "hivemind/ConnectorStore.h"

#include "hivemind/DbUtil.h"

#include <stdexcept>

using namespace hivemind::db;
namespace fs = std::filesystem;

ConnectorStore::ConnectorStore() {
    db_path_ = fs::path{std::getenv("HOME") ? std::getenv("HOME") : "."}
             / ".local" / "share" / "hivemind" / "connectors.db";
    ensure_schema();
}

ConnectorStore::ConnectorStore(std::filesystem::path db) : db_path_(std::move(db)) {
    ensure_schema();
}

ConnectorStore::~ConnectorStore() = default;

sqlite3* ConnectorStore::open_conn() {
    std::error_code ec;
    fs::create_directories(db_path_.parent_path(), ec);
    sqlite3* db = nullptr;
    if (sqlite3_open(db_path_.c_str(), &db) != SQLITE_OK) {
        if (db) sqlite3_close(db);
        throw std::runtime_error("ConnectorStore: cannot open db");
    }
    return db;
}

void ConnectorStore::ensure_schema() {
    sqlite3* db = open_conn();
    exec_sql(db,
        "CREATE TABLE IF NOT EXISTS connectors ("
        "  name TEXT PRIMARY KEY,"
        "  auth_url TEXT NOT NULL,"
        "  token_url TEXT NOT NULL,"
        "  client_id TEXT NOT NULL,"
        "  client_secret TEXT NOT NULL DEFAULT '',"
        "  scopes TEXT NOT NULL DEFAULT '',"
        "  created_at TEXT NOT NULL);"
        "CREATE TABLE IF NOT EXISTS pending_states ("
        "  state TEXT PRIMARY KEY,"
        "  connector_name TEXT NOT NULL,"
        "  created_at TEXT NOT NULL);",
        "ConnectorStore schema");
    sqlite3_close(db);
}

bool ConnectorStore::valid_name(const std::string& n) {
    if (n.empty() || n.size() > 32) return false;
    for (char c : n) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-';
        if (!ok) return false;
    }
    return true;
}

bool ConnectorStore::valid_endpoint(const std::string& url) {
    if (url.rfind("https://", 0) == 0) return url.size() > 8;
    // loopback http only (local dev / mock providers)
    return url.rfind("http://127.0.0.1", 0) == 0 ||
           url.rfind("http://localhost", 0) == 0;
}

bool ConnectorStore::create(const Connector& c) {
    if (!valid_name(c.name))
        throw std::invalid_argument("name must be [a-z0-9-], 1-32 chars");
    if (!valid_endpoint(c.auth_url) || !valid_endpoint(c.token_url))
        throw std::invalid_argument(
            "endpoints must be https:// (http:// only for loopback)");
    if (c.client_id.empty())
        throw std::invalid_argument("client_id is required");
    sqlite3* db = open_conn();
    sqlite3_stmt* st = nullptr;
    const char* sql =
        "INSERT INTO connectors (name, auth_url, token_url, client_id,"
        " client_secret, scopes, created_at) VALUES (?,?,?,?,?,?,?)";
    bool ok = false;
    if (sqlite3_prepare_v2(db, sql, -1, &st, nullptr) == SQLITE_OK) {
        bind_text(st, 1, c.name);
        bind_text(st, 2, c.auth_url);
        bind_text(st, 3, c.token_url);
        bind_text(st, 4, c.client_id);
        bind_text(st, 5, c.client_secret);
        bind_text(st, 6, c.scopes);
        bind_text(st, 7, iso_now());
        ok = sqlite3_step(st) == SQLITE_DONE;
        sqlite3_finalize(st);
    }
    sqlite3_close(db);
    return ok;
}

namespace {
Connector row_to_connector(sqlite3_stmt* st) {
    Connector c;
    c.name          = col_text(st, 0);
    c.auth_url      = col_text(st, 1);
    c.token_url     = col_text(st, 2);
    c.client_id     = col_text(st, 3);
    c.client_secret = col_text(st, 4);
    c.scopes        = col_text(st, 5);
    c.created_at    = col_text(st, 6);
    return c;
}
constexpr const char* kCols =
    "name, auth_url, token_url, client_id, client_secret, scopes, created_at";
} // namespace

std::optional<Connector> ConnectorStore::get(const std::string& name) {
    sqlite3* db = open_conn();
    sqlite3_stmt* st = nullptr;
    std::optional<Connector> out;
    const std::string sql = std::string("SELECT ") + kCols +
                            " FROM connectors WHERE name=?";
    if (sqlite3_prepare_v2(db, sql.c_str(), -1, &st, nullptr) == SQLITE_OK) {
        bind_text(st, 1, name);
        if (sqlite3_step(st) == SQLITE_ROW) out = row_to_connector(st);
        sqlite3_finalize(st);
    }
    sqlite3_close(db);
    return out;
}

std::vector<Connector> ConnectorStore::list() {
    sqlite3* db = open_conn();
    std::vector<Connector> out;
    sqlite3_stmt* st = nullptr;
    const std::string sql = std::string("SELECT ") + kCols +
                            " FROM connectors ORDER BY name";
    if (sqlite3_prepare_v2(db, sql.c_str(), -1, &st, nullptr) == SQLITE_OK) {
        while (sqlite3_step(st) == SQLITE_ROW) out.push_back(row_to_connector(st));
        sqlite3_finalize(st);
    }
    sqlite3_close(db);
    return out;
}

bool ConnectorStore::remove(const std::string& name) {
    sqlite3* db = open_conn();
    sqlite3_stmt* st = nullptr;
    bool ok = false;
    if (sqlite3_prepare_v2(db, "DELETE FROM connectors WHERE name=?",
                           -1, &st, nullptr) == SQLITE_OK) {
        bind_text(st, 1, name);
        ok = sqlite3_step(st) == SQLITE_DONE && sqlite3_changes(db) > 0;
        sqlite3_finalize(st);
    }
    if (ok && sqlite3_prepare_v2(db,
            "DELETE FROM pending_states WHERE connector_name=?",
            -1, &st, nullptr) == SQLITE_OK) {
        bind_text(st, 1, name);
        sqlite3_step(st);
        sqlite3_finalize(st);
    }
    sqlite3_close(db);
    return ok;
}

std::string ConnectorStore::issue_state(const std::string& connector_name) {
    if (!get(connector_name)) return "";
    sqlite3* db = open_conn();
    // opportunistically purge expired states
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db, "DELETE FROM pending_states WHERE created_at < ?",
                           -1, &st, nullptr) == SQLITE_OK) {
        bind_text(st, 1, iso_ago(600));
        sqlite3_step(st);
        sqlite3_finalize(st);
    }
    const std::string state = uuid_v4();
    if (sqlite3_prepare_v2(db,
            "INSERT INTO pending_states (state, connector_name, created_at)"
            " VALUES (?,?,?)", -1, &st, nullptr) == SQLITE_OK) {
        bind_text(st, 1, state);
        bind_text(st, 2, connector_name);
        bind_text(st, 3, iso_now());
        sqlite3_step(st);
        sqlite3_finalize(st);
    }
    sqlite3_close(db);
    return state;
}

std::optional<std::string> ConnectorStore::consume_state(const std::string& state) {
    sqlite3* db = open_conn();
    sqlite3_stmt* st = nullptr;
    std::optional<std::string> name;
    if (sqlite3_prepare_v2(db,
            "SELECT connector_name, created_at FROM pending_states WHERE state=?",
            -1, &st, nullptr) == SQLITE_OK) {
        bind_text(st, 1, state);
        if (sqlite3_step(st) == SQLITE_ROW) {
            const std::string created = col_text(st, 1);
            if (created >= iso_ago(600)) name = col_text(st, 0);
        }
        sqlite3_finalize(st);
    }
    // one-shot either way: a stale or replayed state is dead on arrival
    if (sqlite3_prepare_v2(db, "DELETE FROM pending_states WHERE state=?",
                           -1, &st, nullptr) == SQLITE_OK) {
        bind_text(st, 1, state);
        sqlite3_step(st);
        sqlite3_finalize(st);
    }
    sqlite3_close(db);
    return name;
}
