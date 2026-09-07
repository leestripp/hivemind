// src/LlmStore.cpp — Phase 43: hub-owned provider/model registry.
// See LlmStore.h for the model. Follows the WorkerStore pattern:
// fresh SQLite connection per op, DbUtil helpers.

#include "hivemind/LlmStore.h"
#include "hivemind/DbUtil.h"

#include <stdexcept>

namespace fs = std::filesystem;
using hivemind::db::bind_text;
using hivemind::db::col_text;
using hivemind::db::iso_now;

namespace {

ProviderInfo row_to_provider(sqlite3_stmt* st) {
    ProviderInfo p;
    p.name          = col_text(st, 0);
    p.host          = col_text(st, 1);
    p.api_key       = col_text(st, 2);
    p.chat_endpoint = col_text(st, 3);
    p.plugin        = col_text(st, 4);
    p.created_at    = col_text(st, 5);
    return p;
}

LlmModelInfo row_to_model(sqlite3_stmt* st) {
    LlmModelInfo m;
    m.name       = col_text(st, 0);
    m.provider   = col_text(st, 1);
    m.model_id   = col_text(st, 2);
    m.max_context = sqlite3_column_int(st, 3);
    m.temperature = sqlite3_column_double(st, 4);
    m.created_at = col_text(st, 5);
    return m;
}

constexpr const char* kProviderCols =
    "SELECT name, host, api_key, chat_endpoint, plugin, created_at FROM providers";
constexpr const char* kModelCols =
    "SELECT name, provider, model_id, max_context, temperature, created_at FROM models";

sqlite3* open(const fs::path& path) {
    sqlite3* db = nullptr;
    if (sqlite3_open(path.c_str(), &db) != SQLITE_OK) {
        std::string msg = db ? sqlite3_errmsg(db) : "out of memory";
        if (db) sqlite3_close(db);
        throw std::runtime_error("LlmStore: cannot open db: " + msg);
    }
    sqlite3_busy_timeout(db, 5000);
    return db;
}

struct Db {
    sqlite3* h;
    explicit Db(const fs::path& p) : h(open(p)) {}
    ~Db() { if (h) sqlite3_close(h); }
};

} // namespace

LlmStore::LlmStore() {
    db_path_ = fs::path{std::getenv("HOME") ? std::getenv("HOME") : "."}
             / ".local" / "share" / "hivemind" / "llm.db";
    ensure_schema();
}

LlmStore::LlmStore(fs::path db) : db_path_(std::move(db)) {
    ensure_schema();
}

LlmStore::~LlmStore() = default;

void LlmStore::ensure_schema() {
    std::error_code ec;
    fs::create_directories(db_path_.parent_path(), ec);
    Db db(db_path_);
    // Mirrors the brain's settings tables (Phase 43 design): dispatch
    // injection upserts this shape straight into the brain's store.
    char* err = nullptr;
    sqlite3_exec(db.h,
        "PRAGMA foreign_keys = ON;"
        "CREATE TABLE IF NOT EXISTS providers ("
        "  name TEXT PRIMARY KEY,"
        "  host TEXT NOT NULL DEFAULT '',"
        "  api_key TEXT NOT NULL DEFAULT '',"
        "  chat_endpoint TEXT NOT NULL DEFAULT '',"
        "  plugin TEXT NOT NULL DEFAULT '',"
        "  created_at TEXT NOT NULL"
        ");"
        "CREATE TABLE IF NOT EXISTS models ("
        "  name TEXT PRIMARY KEY,"
        "  provider TEXT NOT NULL REFERENCES providers(name) ON DELETE CASCADE,"
        "  model_id TEXT NOT NULL DEFAULT '',"
        "  max_context INTEGER NOT NULL DEFAULT 0,"
        "  temperature REAL NOT NULL DEFAULT 0,"
        "  created_at TEXT NOT NULL"
        ");"
        "CREATE TABLE IF NOT EXISTS meta ("
        "  key TEXT PRIMARY KEY,"
        "  value TEXT NOT NULL DEFAULT ''"
        ");",
        nullptr, nullptr, &err);
    if (err) {
        std::string msg = err;
        sqlite3_free(err);
        throw std::runtime_error("LlmStore schema: " + msg);
    }
}

// Providers -------------------------------------------------------------------

void LlmStore::upsert_provider(const ProviderInfo& p) {
    if (p.name.empty())
        throw std::invalid_argument("LlmStore: provider name is required");
    Db db(db_path_);
    sqlite3_exec(db.h, "PRAGMA foreign_keys = ON;", nullptr, nullptr, nullptr);
    const char* sql =
        "INSERT INTO providers(name, host, api_key, chat_endpoint, plugin, created_at)"
        " VALUES(?1, ?2, ?3, ?4, ?5, ?6)"
        " ON CONFLICT(name) DO UPDATE SET host=?2, api_key=?3,"
        "   chat_endpoint=?4, plugin=?5;";
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db.h, sql, -1, &st, nullptr) != SQLITE_OK)
        throw std::runtime_error(sqlite3_errmsg(db.h));
    bind_text(st, 1, p.name);
    bind_text(st, 2, p.host);
    bind_text(st, 3, p.api_key);
    bind_text(st, 4, p.chat_endpoint);
    bind_text(st, 5, p.plugin);
    bind_text(st, 6, iso_now());
    bool ok = sqlite3_step(st) == SQLITE_DONE;
    sqlite3_finalize(st);
    if (!ok) throw std::runtime_error(sqlite3_errmsg(db.h));
}

bool LlmStore::remove_provider(const std::string& name) {
    Db db(db_path_);
    sqlite3_exec(db.h, "PRAGMA foreign_keys = ON;", nullptr, nullptr, nullptr);
    const char* sql = "DELETE FROM providers WHERE name = ?1;";
    sqlite3_stmt* st = nullptr;
    sqlite3_prepare_v2(db.h, sql, -1, &st, nullptr);
    bind_text(st, 1, name);
    sqlite3_step(st);
    int changed = sqlite3_changes(db.h);
    sqlite3_finalize(st);
    // Clearing a provider invalidates a default pointing at one of its models.
    return changed > 0;
}

std::optional<ProviderInfo> LlmStore::get_provider(const std::string& name) {
    Db db(db_path_);
    std::string sql = std::string(kProviderCols) + " WHERE name = ?1;";
    sqlite3_stmt* st = nullptr;
    sqlite3_prepare_v2(db.h, sql.c_str(), -1, &st, nullptr);
    bind_text(st, 1, name);
    std::optional<ProviderInfo> out;
    if (sqlite3_step(st) == SQLITE_ROW) out = row_to_provider(st);
    sqlite3_finalize(st);
    return out;
}

std::vector<ProviderInfo> LlmStore::list_providers() {
    Db db(db_path_);
    std::string sql = std::string(kProviderCols) + " ORDER BY name;";
    sqlite3_stmt* st = nullptr;
    sqlite3_prepare_v2(db.h, sql.c_str(), -1, &st, nullptr);
    std::vector<ProviderInfo> out;
    while (sqlite3_step(st) == SQLITE_ROW) out.push_back(row_to_provider(st));
    sqlite3_finalize(st);
    return out;
}

// Models ------------------------------------------------------------------------

void LlmStore::upsert_model(const LlmModelInfo& m) {
    if (m.name.empty() || m.provider.empty())
        throw std::invalid_argument("LlmStore: model name and provider are required");
    if (!get_provider(m.provider))
        throw std::invalid_argument("LlmStore: unknown provider '" + m.provider + "'");
    Db db(db_path_);
    const char* sql =
        "INSERT INTO models(name, provider, model_id, max_context, temperature, created_at)"
        " VALUES(?1, ?2, ?3, ?4, ?5, ?6)"
        " ON CONFLICT(name) DO UPDATE SET provider=?2, model_id=?3,"
        "   max_context=?4, temperature=?5;";
    sqlite3_stmt* st = nullptr;
    sqlite3_prepare_v2(db.h, sql, -1, &st, nullptr);
    bind_text(st, 1, m.name);
    bind_text(st, 2, m.provider);
    bind_text(st, 3, m.model_id.empty() ? m.name : m.model_id);
    sqlite3_bind_int(st, 4, m.max_context);
    sqlite3_bind_double(st, 5, m.temperature);
    bind_text(st, 6, iso_now());
    bool ok = sqlite3_step(st) == SQLITE_DONE;
    sqlite3_finalize(st);
    if (!ok) throw std::runtime_error(sqlite3_errmsg(db.h));
}

bool LlmStore::remove_model(const std::string& name) {
    Db db(db_path_);
    sqlite3_stmt* st = nullptr;
    sqlite3_prepare_v2(db.h, "DELETE FROM models WHERE name = ?1;", -1, &st, nullptr);
    bind_text(st, 1, name);
    sqlite3_step(st);
    int changed = sqlite3_changes(db.h);
    sqlite3_finalize(st);
    if (changed > 0 && default_model() == name) set_default_model("");
    return changed > 0;
}

std::vector<LlmModelInfo> LlmStore::list_models() {
    Db db(db_path_);
    std::string sql = std::string(kModelCols) + " ORDER BY provider, name;";
    sqlite3_stmt* st = nullptr;
    sqlite3_prepare_v2(db.h, sql.c_str(), -1, &st, nullptr);
    std::vector<LlmModelInfo> out;
    while (sqlite3_step(st) == SQLITE_ROW) out.push_back(row_to_model(st));
    sqlite3_finalize(st);
    return out;
}

// Default model ------------------------------------------------------------------

void LlmStore::set_default_model(const std::string& name) {
    if (!name.empty()) {
        bool known = false;
        for (auto& m : list_models()) if (m.name == name) { known = true; break; }
        if (!known)
            throw std::invalid_argument("LlmStore: unknown model '" + name + "'");
    }
    Db db(db_path_);
    sqlite3_stmt* st = nullptr;
    sqlite3_prepare_v2(db.h,
        "INSERT INTO meta(key, value) VALUES('default_model', ?1)"
        " ON CONFLICT(key) DO UPDATE SET value=?1;",
        -1, &st, nullptr);
    bind_text(st, 1, name);
    sqlite3_step(st);
    sqlite3_finalize(st);
}

std::string LlmStore::default_model() {
    Db db(db_path_);
    sqlite3_stmt* st = nullptr;
    sqlite3_prepare_v2(db.h, "SELECT value FROM meta WHERE key='default_model';",
                       -1, &st, nullptr);
    std::string out;
    if (sqlite3_step(st) == SQLITE_ROW) out = col_text(st, 0);
    sqlite3_finalize(st);
    return out;
}

// Dispatch resolution --------------------------------------------------------------

std::optional<ProviderInfo> LlmStore::resolve(const std::string& model_name,
                                              LlmModelInfo* resolved) {
    std::string name = model_name.empty() ? default_model() : model_name;
    if (name.empty()) return std::nullopt;
    for (auto& m : list_models()) {
        if (m.name != name) continue;
        auto p = get_provider(m.provider);
        if (p && resolved) *resolved = m;
        return p;
    }
    return std::nullopt;
}
