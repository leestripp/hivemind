// =============================================================================
// src/AuthStore.cpp — API key authentication (HiveMind 2.0, Plan.md Phase 9)
// =============================================================================
#include "hivemind/AuthStore.h"

#include "token/TokenUtils.h"   // OmniNet: generateApiKey, hashToken

#include <chrono>
#include <cstdio>
#include <iostream>
#include <stdexcept>

namespace fs = std::filesystem;

namespace {

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
        throw std::runtime_error("AuthStore exec failed: " + msg);
    }
}

const char* col_text(sqlite3_stmt* st, int i) {
    const auto* p = sqlite3_column_text(st, i);
    return p ? reinterpret_cast<const char*>(p) : "";
}

} // namespace

AuthStore::AuthStore() {
    db_path_ = fs::path{std::getenv("HOME") ? std::getenv("HOME") : "."}
             / ".local" / "share" / "hivemind" / "auth.db";
    fs::create_directories(db_path_.parent_path());
    ensure_schema();
}

AuthStore::AuthStore(fs::path db) : db_path_(std::move(db)) {
    fs::create_directories(db_path_.parent_path());
    ensure_schema();
}

AuthStore::~AuthStore() = default;

sqlite3* AuthStore::open_conn() {
    sqlite3* db = nullptr;
    if (sqlite3_open(db_path_.string().c_str(), &db) != SQLITE_OK) {
        std::string msg = db ? sqlite3_errmsg(db) : "out of memory";
        if (db) sqlite3_close(db);
        throw std::runtime_error("AuthStore open failed: " + msg);
    }
    return db;
}

void AuthStore::ensure_schema() {
    sqlite3* db = open_conn();
    exec_sql(db,
        "CREATE TABLE IF NOT EXISTS api_keys ("
        "  id TEXT PRIMARY KEY,"
        "  name TEXT NOT NULL,"
        "  key_hash TEXT NOT NULL UNIQUE,"
        "  scopes TEXT NOT NULL DEFAULT 'agent',"
        "  created_at TEXT NOT NULL,"
        "  revoked INTEGER NOT NULL DEFAULT 0"
        ")");
    sqlite3_close(db);
}

std::vector<std::string> AuthStore::split_scopes(const std::string& csv) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : csv) {
        if (c == ',' || c == ' ') {
            if (!cur.empty()) { out.push_back(cur); cur.clear(); }
        } else cur += c;
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

bool AuthStore::has_scope(const std::vector<std::string>& scopes,
                          const std::string& need) {
    for (auto& s : scopes)
        if (s == need || s == "admin") return true;   // admin implies all
    return false;
}

bool AuthStore::ensure_master_key(bool print) {
    if (!list().empty()) return false;
    std::string raw = create_key("master", "admin");
    if (print) {
        std::cout << "\n"
            "════════════════════════════════════════════════════════════\n"
            "  HiveMind master API key (shown ONCE — save it now):\n\n"
            "    " << raw << "\n\n"
            "  Every /api/* call needs:  Authorization: Bearer <key>\n"
            "  Create more keys with:    POST /api/keys\n"
            "════════════════════════════════════════════════════════════\n"
                  << std::endl;
    }
    return true;
}

std::string AuthStore::create_key(const std::string& name,
                                  const std::string& scopes) {
    std::string raw = OmniNet::Token::generateApiKey("hm");
    std::string hash = OmniNet::Token::hashToken(raw);
    // Public id: key prefix for display/revocation (never the secret part).
    std::string id = raw.substr(0, raw.find('_', 3) == std::string::npos
                                      ? 12 : raw.find('_', 3));

    sqlite3* db = open_conn();
    sqlite3_stmt* st = nullptr;
    const char* sql =
        "INSERT INTO api_keys (id, name, key_hash, scopes, created_at, revoked)"
        " VALUES (?,?,?,?,?,0)";
    if (sqlite3_prepare_v2(db, sql, -1, &st, nullptr) != SQLITE_OK) {
        std::string msg = sqlite3_errmsg(db);
        sqlite3_close(db);
        throw std::runtime_error("AuthStore insert prepare failed: " + msg);
    }
    bind_text(st, 1, id);
    bind_text(st, 2, name);
    bind_text(st, 3, hash);
    bind_text(st, 4, scopes.empty() ? "agent" : scopes);
    bind_text(st, 5, iso_now());
    bool ok = sqlite3_step(st) == SQLITE_DONE;
    sqlite3_finalize(st);
    sqlite3_close(db);
    if (!ok) throw std::runtime_error("AuthStore insert failed");
    return raw;
}

std::optional<std::vector<std::string>>
AuthStore::validate(const std::string& raw_key) {
    if (raw_key.empty()) return std::nullopt;
    std::string hash = OmniNet::Token::hashToken(raw_key);

    sqlite3* db = open_conn();
    sqlite3_stmt* st = nullptr;
    std::optional<std::vector<std::string>> out;
    if (sqlite3_prepare_v2(db,
            "SELECT scopes FROM api_keys WHERE key_hash = ? AND revoked = 0",
            -1, &st, nullptr) == SQLITE_OK) {
        bind_text(st, 1, hash);
        if (sqlite3_step(st) == SQLITE_ROW)
            out = split_scopes(col_text(st, 0));
        sqlite3_finalize(st);
    }
    sqlite3_close(db);
    return out;
}

bool AuthStore::revoke(const std::string& id) {
    sqlite3* db = open_conn();
    sqlite3_stmt* st = nullptr;
    bool ok = false;
    if (sqlite3_prepare_v2(db,
            "UPDATE api_keys SET revoked = 1 WHERE id = ? AND revoked = 0",
            -1, &st, nullptr) == SQLITE_OK) {
        bind_text(st, 1, id);
        ok = sqlite3_step(st) == SQLITE_DONE && sqlite3_changes(db) > 0;
        sqlite3_finalize(st);
    }
    sqlite3_close(db);
    return ok;
}

std::vector<ApiKeyEntry> AuthStore::list() {
    sqlite3* db = open_conn();
    std::vector<ApiKeyEntry> out;
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db,
            "SELECT id, name, scopes, created_at, revoked FROM api_keys"
            " ORDER BY created_at ASC",
            -1, &st, nullptr) == SQLITE_OK) {
        while (sqlite3_step(st) == SQLITE_ROW) {
            ApiKeyEntry e;
            e.id = col_text(st, 0);
            e.name = col_text(st, 1);
            e.scopes = col_text(st, 2);
            e.created_at = col_text(st, 3);
            e.revoked = sqlite3_column_int(st, 4) != 0;
            out.push_back(std::move(e));
        }
        sqlite3_finalize(st);
    }
    sqlite3_close(db);
    return out;
}
