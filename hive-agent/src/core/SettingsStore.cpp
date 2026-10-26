// SPDX-License-Identifier: BSL-1.0
// src/core/SettingsStore.cpp
//
// SQLite-backed settings store for the engine. Backed by OmniNet's
// `DB::Database` (which itself wraps sqlite3) — we do NOT directly call
// sqlite3_* APIs from hive-brain. That keeps the SQLite version, error
// reporting, thread-safety policy, and prepared-statement caching in one
// place (OmniNet) instead of duplicating it here.
//
// Stored at:
//   $XDG_DATA_HOME/hive-brain/hive-agent.db        (default)
//       └─ ~/.local/share/hive-brain/hive-agent.db
//   <home>/hive-agent.db                          (per-agent override)
//
// Schema is created idempotently on first open. `schema_version` table
// keeps migrations straightforward.
//
// §B1 fix: every public method that takes a string argument now uses a
// prepared statement with `?` placeholders + `Statement::bind(...)`. No
// user-controlled value is ever concatenated into a SQL string.
//
// §B2 fix (v0.5.1): SettingsStore is no longer pimpl'd. The Impl fields
// (path_ + db_) are inlined into the class, making it copyable/movable.
// for_home() returns by value; the DbPool entry outlives all copies.

#include "core/settings.h"

#include <cassert>
#include <chrono>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

#include <nlohmann/json.hpp>
#include <db/Database.h>      // OmniNet::DB::Database — sqlite3 wrapper
#include <sqlite3.h>          // sqlite3_errcode / sqlite3_errmsg for step checks

#include "library/paths.h"   // Paths::global() for the default DB path
#include "api/ProviderPlugin.h"
#include "api/ProviderRegistry.h"
#include "core/Logger.h"

namespace fs = std::filesystem;

namespace {

// Per-DB-path mutex pool. SettingsStore copies for the same DB path share the
// same mutex so that prepared-statement access is serialised across all
// handles that point at the same SQLite file.
std::shared_ptr<std::recursive_mutex> mutex_for_path(const fs::path& p)
{
    static std::mutex mu;
    static std::unordered_map<std::string, std::weak_ptr<std::recursive_mutex>> map;

    std::lock_guard<std::mutex> lk(mu);
    const std::string key = p.lexically_normal().string();
    auto it = map.find(key);
    if (it != map.end()) {
        if (auto sp = it->second.lock()) {
            return sp;
        }
    }
    auto sp = std::make_shared<std::recursive_mutex>();
    map[key] = sp;
    return sp;
}

// Throw if the last SQLite result on this handle is an actual error (not
// SQLITE_DONE or SQLITE_OK). OmniNet's Statement::step() returns false for
// both "no more rows" and any error, so callers must check the handle error
// state explicitly to avoid silently ignoring failed writes.
void throw_on_step_error(OmniNet::DB::Database* db, int allowed = SQLITE_DONE)
{
    if (!db || !db->handle()) return;
    const int rc = sqlite3_errcode(db->handle());
    if (rc != SQLITE_OK && rc != allowed) {
        const char* msg = sqlite3_errmsg(db->handle());
        throw OmniNet::DB::DatabaseException(
            std::string("SQLite step failed: ") + (msg ? msg : "unknown"), rc);
    }
}

}  // anonymous namespace

namespace hive {

// ============================================================================
// Built-in max_context_size defaults for known models
// ============================================================================
namespace {
int builtin_max_context_size(std::string_view model_name)
{
    // Map is keyed by the settings name (e.g. "minimax-m2.7") so the user
    // can also match by the model id (e.g. "MiniMax-M2.7") if needed.
    // Entries are (setting_name, context_size) pairs.
    static const std::pair<std::string_view, int> known_models[] = {
        // MiniMax models
        {"minimax-m3",      500000},  // 500K tokens (actual model limit)
        {"MiniMax-M3",     500000},
        {"minimax-m2.7",    200000},  // 200K tokens
        {"MiniMax-M2.7",   200000},
        {"minimax",         500000},  // Default MiniMax alias
        {"minimax-m2",      200000},  // Default MiniMax-M2 alias
        // OpenAI models
        {"gpt-4",           8192},
        {"gpt-4-32k",       32768},
        {"gpt-4-turbo",     128000},
        {"gpt-4o",          128000},
        {"gpt-4o-mini",     128000},
        {"gpt-3.5-turbo",    16385},
        {"o1",              200000},
        {"o1-mini",         128000},
        {"o3-mini",         200000},
        // Anthropic models
        {"claude-3-opus",       200000},
        {"claude-3-sonnet",     200000},
        {"claude-3-haiku",      200000},
        {"claude-3.5-sonnet",   200000},
        {"claude-3.5-haiku",    200000},
        // DeepSeek models
        {"deepseek-chat",       128000},
        {"deepseek-coder",      128000},
    };

    for (const auto& [name, size] : known_models) {
        if (name == model_name) return size;
    }
    return 0;  // 0 means "unknown" — caller applies the 1M fallback
}

// §ProviderPlugins: resolve the plugin for a provider row. The explicit
// `plugin` hint wins; otherwise auto-detect from name/host (resolve()
// always falls back to the OpenAI-compatible default, so this never
// returns nullptr).
std::shared_ptr<ProviderPlugin> plugin_for_row(std::string_view hint,
                                               std::string_view name,
                                               std::string_view host)
{
    if (!hint.empty()) {
        if (auto pl = ProviderRegistry::plugin_for_name(hint)) return pl;
    }
    return ProviderRegistry::resolve(name, host);
}
}  // anonymous namespace

// ============================================================================
// Schema
// ============================================================================
namespace {

constexpr int kSchemaVersion = 6;

constexpr const char* kSchemaSql = R"sql(
CREATE TABLE IF NOT EXISTS schema_version (
    version INTEGER PRIMARY KEY
);

CREATE TABLE IF NOT EXISTS providers (
    name               TEXT PRIMARY KEY,
    host               TEXT NOT NULL,
    api_key            TEXT NOT NULL,
    chat_endpoint      TEXT NOT NULL DEFAULT '/v1/chat/completions',
    discovery_endpoint TEXT NOT NULL DEFAULT '/v1/models',
    timeout_seconds    INTEGER NOT NULL DEFAULT 30,
    -- §ProviderPlugins: registered provider-plugin name; '' = auto-detect
    -- from provider name/host at runtime.
    plugin             TEXT NOT NULL DEFAULT ''
);

CREATE TABLE IF NOT EXISTS models (
    name             TEXT PRIMARY KEY,
    provider         TEXT NOT NULL REFERENCES providers(name),
    model_id         TEXT NOT NULL,
    max_context_size INTEGER NOT NULL DEFAULT 0,
    temperature      REAL    NOT NULL DEFAULT 0.0
);

CREATE TABLE IF NOT EXISTS kv_settings (
    key   TEXT PRIMARY KEY,
    value TEXT NOT NULL
);

-- §v0.5.0: schema v2 adds description, bearer_token, enabled for full
-- parity with McpServerConfig. Pre-v2 DBs get an in-place ALTER TABLE
-- migration below.
CREATE TABLE IF NOT EXISTS mcp_servers (
    name        TEXT PRIMARY KEY,
    transport   TEXT NOT NULL DEFAULT 'stdio',
    command     TEXT,
    args_json   TEXT,
    env_json    TEXT,
    url         TEXT,
    bearer_token TEXT,
    description TEXT,
    enabled     INTEGER NOT NULL DEFAULT 1,
    origin      TEXT NOT NULL DEFAULT 'local'
);

)sql";

/// Per-version migrations. Called when schema_version table reports
/// a value less than kSchemaVersion. Each entry bumps v → v+1.
void migrate_to_v2(OmniNet::DB::Database& db)
{
    // v2: add mcp_servers.description / bearer_token / enabled.
    // ALTER TABLE columns; idempotent (catch "duplicate column" errors).
    // Column names are constants — no user input flows here, raw execute()
    // is safe.
    auto add_col = [&](const std::string& col, const std::string& def) {
        try {
            db.execute("ALTER TABLE mcp_servers ADD COLUMN " + col + " " + def);
        } catch (...) {
            // Column already exists — fine.
        }
    };
    add_col("bearer_token", "TEXT");
    add_col("description",  "TEXT");
    add_col("enabled",      "INTEGER NOT NULL DEFAULT 1");
}

void migrate_to_v3(OmniNet::DB::Database& db)
{
    // v3: add models.provider for grouping models by provider label.
    try {
        db.execute("ALTER TABLE models ADD COLUMN provider TEXT NOT NULL DEFAULT ''");
    } catch (...) {
        // Column already exists — fine.
    }
}

void migrate_to_v4(OmniNet::DB::Database& db)
{
    // v4: split provider-level settings into a separate providers table.
    // Models keep only model-specific settings and reference providers by name.
    db.execute(
        "CREATE TABLE IF NOT EXISTS providers ("
        "name TEXT PRIMARY KEY, "
        "host TEXT NOT NULL, "
        "api_key TEXT NOT NULL, "
        "chat_endpoint TEXT NOT NULL DEFAULT '/v1/chat/completions', "
        "discovery_endpoint TEXT NOT NULL DEFAULT '/v1/models', "
        "timeout_seconds INTEGER NOT NULL DEFAULT 30)");

    // Migrate existing models that still have provider-level columns.
    // We detect the old schema by checking whether the 'host' column exists.
    bool has_old_columns = false;
    try {
        db.query("PRAGMA table_info(models)", [&](const std::vector<std::string>& row) {
            if (row.size() >= 2 && row[1] == "host") {
                has_old_columns = true;
                return false; // stop iterating
            }
            return true;
        });
    } catch (...) {
    }

    if (has_old_columns) {
        // Create a provider for each distinct provider label.
        db.query(
            "SELECT DISTINCT provider, host, api_key, chat_endpoint "
            "FROM models WHERE host <> '' AND provider <> ''",
            [&](const std::vector<std::string>& row) {
                if (row.size() < 4) return true;
                std::string provider_name = row[0].empty() ? "legacy" : row[0];
                std::string host = row[1];
                std::string api_key = row[2];
                std::string chat_endpoint = row[3].empty() ? "/v1/chat/completions" : row[3];

                auto stmt = db.prepare(
                    "INSERT OR IGNORE INTO providers(name, host, api_key, chat_endpoint) "
                    "VALUES(?, ?, ?, ?)");
                stmt.bind(1, provider_name);
                stmt.bind(2, host);
                stmt.bind(3, api_key);
                stmt.bind(4, chat_endpoint);
                stmt.step();
                return true;
            });

        // For models whose provider label has no row, synthesize one per model.
        db.query(
            "SELECT name, host, api_key, chat_endpoint FROM models "
            "WHERE provider = '' OR provider NOT IN (SELECT name FROM providers)",
            [&](const std::vector<std::string>& row) {
                if (row.size() < 4) return true;
                std::string provider_name = "legacy-" + row[0];
                std::string host = row[1];
                std::string api_key = row[2];
                std::string chat_endpoint = row[3].empty() ? "/v1/chat/completions" : row[3];

                {
                    auto stmt = db.prepare(
                        "INSERT OR IGNORE INTO providers(name, host, api_key, chat_endpoint) "
                        "VALUES(?, ?, ?, ?)");
                    stmt.bind(1, provider_name);
                    stmt.bind(2, host);
                    stmt.bind(3, api_key);
                    stmt.bind(4, chat_endpoint);
                    stmt.step();
                }

                {
                    auto stmt = db.prepare("UPDATE models SET provider = ? WHERE name = ?");
                    stmt.bind(1, provider_name);
                    stmt.bind(2, row[0]);
                    stmt.step();
                }
                return true;
            });

        // Re-create models table without provider-level columns.
        db.execute(
            "CREATE TABLE models_new ("
            "name TEXT PRIMARY KEY, "
            "provider TEXT NOT NULL REFERENCES providers(name), "
            "model_id TEXT NOT NULL, "
            "max_context_size INTEGER NOT NULL DEFAULT 0, "
            "temperature REAL NOT NULL DEFAULT 0.0)");

        db.execute(
            "INSERT INTO models_new(name, provider, model_id, max_context_size, temperature) "
            "SELECT m.name, "
            "COALESCE((SELECT p.name FROM providers p "
            "          WHERE p.name = m.provider AND m.provider <> ''), "
            "         'legacy-' || m.name), "
            "m.model_id, m.max_context_size, m.temperature "
            "FROM models m");

        db.execute("DROP TABLE models");
        db.execute("ALTER TABLE models_new RENAME TO models");
    }
}

void migrate_to_v5(OmniNet::DB::Database& db)
{
    // v5 (§ProviderPlugins): providers.plugin — registered provider-plugin
    // name. '' = auto-detect from provider name/host at runtime, so
    // existing rows automatically pick up built-in plugins as they ship.
    try {
        db.execute("ALTER TABLE providers ADD COLUMN plugin TEXT NOT NULL DEFAULT ''");
    } catch (...) {
        // Column already exists — fine.
    }
}

void migrate_to_v6(OmniNet::DB::Database& db)
{
    // v6 (Phase 71): mcp_servers.origin — 'hub' rows are owned by the
    // roster seed (hub-authoritative, read-only to the agent); 'local'
    // rows are agent self-service (mcp_add) and survive re-seeds.
    // Existing rows predate self-service: every one of them came from
    // the boot seed, so mark them 'hub'.
    try {
        db.execute("ALTER TABLE mcp_servers ADD COLUMN origin TEXT NOT NULL DEFAULT 'local'");
        db.execute("UPDATE mcp_servers SET origin = 'hub'");
    } catch (...) {
        // Column already exists — fine.
    }
}

void ensure_schema(OmniNet::DB::Database& db)
{
    db.execute(kSchemaSql);

    // schema_version row. The constants here are integer literals — no
    // user input. Raw execute() is safe.
    int current_version = 0;
    auto row = db.queryOne("SELECT version FROM schema_version LIMIT 1");
    if (row.has_value() && !row->empty()) {
        try {
            current_version = std::stoi((*row)[0]);
        } catch (...) {
            current_version = 0;
        }
    } else {
        db.execute("INSERT INTO schema_version(version) VALUES (" +
                   std::to_string(kSchemaVersion) + ")");
        current_version = kSchemaVersion;
    }

    // Run any pending per-version migrations.
    if (current_version < 2) migrate_to_v2(db);
    if (current_version < 3) migrate_to_v3(db);
    if (current_version < 4) migrate_to_v4(db);
    if (current_version < 5) migrate_to_v5(db);
    if (current_version < 6) migrate_to_v6(db);

    // Update the stored version if it advanced.
    if (current_version != kSchemaVersion) {
        // SQLite's UPSERT pattern. Set to current version.
        db.execute("DELETE FROM schema_version");
        db.execute("INSERT INTO schema_version(version) VALUES (" +
                   std::to_string(kSchemaVersion) + ")");
    }
}

ProviderConfig row_to_provider(const std::vector<std::string>& r)
{
    // SELECT order: name, host, api_key, chat_endpoint, discovery_endpoint,
    // timeout_seconds, plugin
    ProviderConfig p;
    if (r.size() < 6) return p;
    p.name               = r[0];
    p.host               = r[1];
    p.api_key            = r[2];
    p.chat_endpoint      = r[3];
    p.discovery_endpoint = r[4];
    try {
        p.timeout_seconds = std::stoi(r[5]);
    } catch (...) {
        p.timeout_seconds = 30;
    }
    if (r.size() >= 7) p.plugin = r[6];  // §ProviderPlugins
    return p;
}

ModelConfig row_to_model(const std::vector<std::string>& r)
{
    // SELECT order: name, provider, model_id, max_context_size, temperature
    ModelConfig m;
    if (r.size() < 5) return m;
    m.name             = r[0];
    m.provider         = r[1];
    m.model_id         = r[2];
    try {
        m.max_context_size = std::stoi(r[3]);
        m.temperature      = std::stod(r[4]);
    } catch (...) {
        m.max_context_size = 0;
        m.temperature      = 0.0;
    }
    return m;
}

}  // anonymous namespace

// ============================================================================
// Prepared-statement helpers (§B1 fix)
// ============================================================================
//
// Every public method on SettingsStore that takes a string parameter
// routes through these helpers instead of concatenating into a SQL
// string. Bind accepts int, int, double, std::string, std::string_view
// — overloads on the argument type do the right thing.
//
// The fold expression `(stmt.bind(idx++, arg), ...)` expands left-to-right
// so idx increments cleanly across positions 1..N.
namespace {

template <typename... Args>
void bind_args(OmniNet::DB::Database::Statement& stmt, Args&&... args)
{
    int idx = 1;
    (stmt.bind(idx++, std::forward<Args>(args)), ...);
}

/// Execute a prepared statement (no result rows). Used for INSERT /
/// UPDATE / DELETE. Locks the per-path mutex and verifies that step()
/// finished with SQLITE_DONE rather than an error code.
template <typename... Args>
void exec_prepared(std::recursive_mutex& mu,
                   OmniNet::DB::Database& db,
                   const std::string& sql,
                   Args&&... args)
{
    std::lock_guard<std::recursive_mutex> lk(mu);
    auto stmt = db.prepare(sql);
    bind_args(stmt, std::forward<Args>(args)...);
    stmt.step();  // INSERT/UPDATE/DELETE — discard the "no row" return.
    throw_on_step_error(&db, SQLITE_DONE);
}

/// Query first row via a prepared statement.
template <typename... Args>
std::optional<std::vector<std::string>>
query_one_prepared(std::recursive_mutex& mu,
                   OmniNet::DB::Database& db,
                   const std::string& sql,
                   Args&&... args)
{
    std::lock_guard<std::recursive_mutex> lk(mu);
    auto stmt = db.prepare(sql);
    bind_args(stmt, std::forward<Args>(args)...);
    if (!stmt.step()) {
        throw_on_step_error(&db, SQLITE_DONE);
        return std::nullopt;
    }
    std::vector<std::string> row;
    for (int i = 0; i < stmt.columnCount(); ++i) {
        row.push_back(stmt.columnText(i));
    }
    return row;
}

/// Query all rows via a prepared statement.
std::vector<std::vector<std::string>>
query_all_prepared(std::recursive_mutex& mu,
                   OmniNet::DB::Database& db, const std::string& sql)
{
    std::lock_guard<std::recursive_mutex> lk(mu);
    std::vector<std::vector<std::string>> rows;
    auto stmt = db.prepare(sql);
    while (stmt.step()) {
        std::vector<std::string> row;
        for (int i = 0; i < stmt.columnCount(); ++i) {
            row.push_back(stmt.columnText(i));
        }
        rows.push_back(std::move(row));
    }
    throw_on_step_error(&db, SQLITE_DONE);
    return rows;
}

}  // anonymous namespace (prepared-statement helpers)

// ============================================================================
// Per-path singleton DB pool. One sqlite3 handle per unique db_path so
// a SettingsStore copy (or two for_home(home) calls) shares the same
// connection. Opens lazily on first use.
// ============================================================================
class DbPool
{
public:
    static DbPool& instance()
    {
        static DbPool p;
        return p;
    }

    OmniNet::DB::Database& get(const fs::path& path)
    {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = dbs_.find(path);
        // If the cached handle's file was removed out from under us,
        // drop the stale handle and reopen. (Real users stop hive-brain
        // before nuking the DB — but tests delete the file constantly.)
        if (it != dbs_.end() && !fs::exists(path)) {
            dbs_.erase(it);
            it = dbs_.end();
        }
        if (it == dbs_.end()) {
            // Open + migrate + store.
            fs::create_directories(path.parent_path());
            auto db = std::make_unique<OmniNet::DB::Database>();
            db->open(path.string());
            db->setBusyTimeout(std::chrono::milliseconds(5000));
            ensure_schema(*db);
            it = dbs_.emplace(path, std::move(db)).first;
        }
        return *it->second;
    }

private:
    std::mutex mu_;
    std::unordered_map<fs::path, std::unique_ptr<OmniNet::DB::Database>> dbs_;
};

// ============================================================================
// SettingsStore ctor
// ============================================================================

// §B2 fix (v0.5.1): Impl fields (path_ + db_) are now inlined into the
// class. This constructor directly initialises those members instead of
// allocating a separate Impl struct.
//
// Note: the second constructor argument is kept for source compatibility
// but has no effect — auto-migration was removed in v0.5.2.
SettingsStore::SettingsStore(fs::path db_path, bool /*auto_migrate*/) {
    path_  = std::move(db_path);
    mutex_ = mutex_for_path(path_);
    db_    = &DbPool::instance().get(path_);
}
// ============================================================================
// instance() / for_home()
// ============================================================================
fs::path default_db_path()
{
    // XDG_DATA_HOME/hive-brain/hive-agent.db (default
    // ~/.local/share/hive-brain/hive-agent.db) — matches the plan.
    fs::path dir = fs::path(Paths::global().user_data_root());
    fs::create_directories(dir);
    fs::path db = dir / "hive-agent.db";
    // Phase 43: migrate daedalus.db → hive-agent.db.
    std::error_code ec;
    fs::path old_db = dir / "daedalus.db";
    if (!fs::exists(db, ec) && fs::exists(old_db, ec))
        fs::rename(old_db, db, ec);
    return db;
}

SettingsStore& SettingsStore::instance()
{
    // v0.5.0: no auto-migration — the DB is the source of truth.
    // Populate it via the CLI or API before first use. (The migration
    // was a one-time event; there is no ongoing JSON→SQLite sync.)
    static SettingsStore s(default_db_path(), /*auto_migrate=*/false);
    return s;
}

SettingsStore SettingsStore::for_home(fs::path home)
{
    // §B2 fix (v0.5.1): return by value instead of leaking a heap allocation.
    //
    // The DbPool entry (the actual sqlite3 handle) lives for the process
    // duration. The returned SettingsStore wrapper is move-constructed from
    // the local temporary; when the caller's variable goes out of scope,
    // only the lightweight path_+db_ pair is destroyed — the DbPool entry
    // is untouched.
    //
    // auto_migrate=false: tests that call for_home() on temp paths must
    // not accidentally migrate user-data JSON files.
    fs::create_directories(home);
    // Phase 43: migrate daedalus.db → hive-agent.db inside the home dir.
    std::error_code ec;
    fs::path old_db = home / "hive-agent.db", new_db = home / "hive-agent.db";
    if (!fs::exists(new_db, ec) && fs::exists(old_db, ec))
        fs::rename(old_db, new_db, ec);
    return SettingsStore(new_db, /*auto_migrate=*/false);
}

// ============================================================================
// Read
// ============================================================================
std::optional<ProviderConfig> SettingsStore::get_provider(std::string_view name) const
{
    auto row = query_one_prepared(
        *mutex_,
        *db_,
        "SELECT name, host, api_key, chat_endpoint, discovery_endpoint, "
        "timeout_seconds, plugin FROM providers WHERE name = ?",
        std::string(name));
    if (!row.has_value()) return std::nullopt;
    return row_to_provider(*row);
}

std::vector<ProviderConfig> SettingsStore::list_providers_detailed() const
{
    std::vector<ProviderConfig> out;
    auto rows = query_all_prepared(
        *mutex_,
        *db_,
        "SELECT name, host, api_key, chat_endpoint, discovery_endpoint, "
        "timeout_seconds, plugin FROM providers ORDER BY name");
    out.reserve(rows.size());
    for (const auto& row : rows) {
        out.push_back(row_to_provider(row));
    }
    return out;
}

std::optional<ModelConfig> SettingsStore::get_model(std::string_view name) const
{
    auto row = query_one_prepared(
        *mutex_,
        *db_,
        "SELECT name, provider, model_id, max_context_size, temperature "
        "FROM models WHERE name = ?",
        std::string(name));
    if (!row.has_value()) return std::nullopt;
    return row_to_model(*row);
}

std::vector<ModelConfig> SettingsStore::list_models() const
{
    std::vector<ModelConfig> out;
    auto rows = query_all_prepared(
        *mutex_,
        *db_,
        "SELECT name, provider, model_id, max_context_size, temperature FROM models");
    out.reserve(rows.size());
    for (const auto& row : rows) {
        out.push_back(row_to_model(row));
    }
    return out;
}

std::optional<ResolvedModelConfig> SettingsStore::resolve_model(std::string_view name) const
{
    auto m = get_model(name);
    if (!m.has_value()) return std::nullopt;

    auto p = get_provider(m->provider);
    if (!p.has_value()) return std::nullopt;

    ResolvedModelConfig out;
    out.name               = m->name;
    out.provider           = m->provider;
    out.model_id           = m->model_id;
    out.host               = p->host;
    out.api_key            = p->api_key;
    // §ProviderPlugins: rows written before endpoint defaults were
    // enforced (or by hosts that leave the fields blank) inherit the
    // plugin's endpoints instead of breaking the request URL.
    const auto plugin = plugin_for_row(p->plugin, p->name, p->host);
    out.chat_endpoint      = p->chat_endpoint.empty()
                                 ? plugin->chat_endpoint() : p->chat_endpoint;
    out.discovery_endpoint = p->discovery_endpoint.empty()
                                 ? plugin->models_endpoint() : p->discovery_endpoint;
    out.timeout_seconds    = p->timeout_seconds;
    out.max_context_size   = m->max_context_size;
    out.temperature        = m->temperature;
    out.plugin             = p->plugin;  // §ProviderPlugins
    return out;
}

std::string SettingsStore::get_string(std::string_view key, std::string_view def) const
{
    auto row = query_one_prepared(
        *mutex_,
        *db_,
        "SELECT value FROM kv_settings WHERE key = ?",
        std::string(key));
    if (!row.has_value()) return std::string(def);
    return row->empty() ? std::string(def) : (*row)[0];
}

bool SettingsStore::get_bool(std::string_view key, bool def) const
{
    auto v = get_string(key, "");
    if (v.empty()) return def;
    return v == "1" || v == "true" || v == "yes";
}

int SettingsStore::get_int(std::string_view key, int def) const
{
    auto v = get_string(key, "");
    if (v.empty()) return def;
    try { return std::stoi(v); } catch (...) { return def; }
}

// ============================================================================
// Write
// ============================================================================
void SettingsStore::upsert_provider(const ProviderConfig& p)
{
    // §ProviderPlugins: empty endpoint fields inherit the plugin's
    // defaults. The SQL column DEFAULTs only apply to omitted columns —
    // hosts legitimately leave these blank (e.g. CLI /provider-add), and
    // storing "" would break request URLs downstream.
    const auto plugin = plugin_for_row(p.plugin, p.name, p.host);
    const std::string chat = p.chat_endpoint.empty()
                                 ? plugin->chat_endpoint() : p.chat_endpoint;
    const std::string disc = p.discovery_endpoint.empty()
                                 ? plugin->models_endpoint() : p.discovery_endpoint;
    exec_prepared(
        *mutex_,
        *db_,
        "INSERT INTO providers(name, host, api_key, chat_endpoint, discovery_endpoint, "
        "timeout_seconds, plugin) VALUES(?, ?, ?, ?, ?, ?, ?) "
        "ON CONFLICT(name) DO UPDATE SET "
        "host=excluded.host, api_key=excluded.api_key, "
        "chat_endpoint=excluded.chat_endpoint, "
        "discovery_endpoint=excluded.discovery_endpoint, "
        "timeout_seconds=excluded.timeout_seconds, "
        "plugin=excluded.plugin",
        p.name, p.host, p.api_key, chat, disc,
        static_cast<int64_t>(p.timeout_seconds), p.plugin);
}

void SettingsStore::delete_provider(std::string_view name)
{
    if (!db_) return;

    // §webui-provider-delete: providers are referenced by models via a
    // foreign key. SQLite in this build enforces that key, so deleting a
    // provider that still has models silently fails. Cascade the delete by
    // removing the dependent models first, then the provider.
    std::lock_guard<std::recursive_mutex> lk(*mutex_);
    db_->transaction([this, name]() {
        auto stmt = db_->prepare("SELECT name FROM models WHERE provider = ?");
        stmt.bind(1, std::string(name));
        std::vector<std::string> dependent_models;
        while (stmt.step()) {
            dependent_models.push_back(stmt.columnText(0));
        }
        for (const auto& model_name : dependent_models) {
            delete_model(model_name);
        }
        exec_prepared(
            *mutex_,
            *db_,
            "DELETE FROM providers WHERE name = ?",
            std::string(name));
    });
}

void SettingsStore::upsert_model(const ModelConfig& m)
{
    exec_prepared(
        *mutex_,
        *db_,
        "INSERT INTO models(name, provider, model_id, max_context_size, temperature) "
        "VALUES(?, ?, ?, ?, ?) "
        "ON CONFLICT(name) DO UPDATE SET "
        "provider=excluded.provider, model_id=excluded.model_id, "
        "max_context_size=excluded.max_context_size, "
        "temperature=excluded.temperature",
        m.name, m.provider, m.model_id,
        static_cast<int64_t>(m.max_context_size),
        m.temperature);
}

void SettingsStore::delete_model(std::string_view name)
{
    exec_prepared(
        *mutex_,
        *db_,
        "DELETE FROM models WHERE name = ?",
        std::string(name));
}

void SettingsStore::set_string(std::string_view key, std::string_view value)
{
    exec_prepared(
        *mutex_,
        *db_,
        "INSERT INTO kv_settings(key, value) VALUES(?, ?) "
        "ON CONFLICT(key) DO UPDATE SET value=excluded.value",
        std::string(key), std::string(value));
}

void SettingsStore::set_bool(std::string_view key, bool value)
{
    set_string(key, value ? "1" : "0");
}

void SettingsStore::set_int(std::string_view key, int value)
{
    set_string(key, std::to_string(value));
}

// ============================================================================
// Atomic
// ============================================================================
void SettingsStore::transaction(std::function<void(SettingsStore&)> fn)
{
    // OmniNet::DB::Database::transaction takes a no-arg lambda but our
    // API needs to pass `*this` so the callback can chain. Trampoline.
    std::lock_guard<std::recursive_mutex> lk(*mutex_);
    SettingsStore& self = *this;
    db_->transaction([&self, &fn]() { fn(self); });
}

// ============================================================================
// Introspection
// ============================================================================
fs::path SettingsStore::db_path() const noexcept { return path_; }

bool SettingsStore::has_data() const noexcept
{
    // §fix (2026-07-18): SQLite prepare/step can throw; this function is
    // noexcept, so an escaping exception would call std::terminate.
    try {
        std::lock_guard<std::recursive_mutex> lk(*mutex_);
        if (!list_models().empty()) return true;
        // Cheap kv_settings probe — one row, one column, no bindings.
        auto stmt = db_->prepare("SELECT 1 FROM kv_settings LIMIT 1");
        return stmt.step();
    } catch (...) {
        return false;
    }
}

// ============================================================================
// v0.3.5: per-project persistence (session memory + messages)
// ============================================================================
//
// Both are stored as kv_settings under namespaced keys ("mem.<sid>",
// "msg.<sid>") — same table as scalars, no schema change needed.
// Just append-only logic on top of the existing set_string/get_string.

std::string SettingsStore::get_session_memory(std::string_view session_id) const
{
    return get_string("mem." + std::string(session_id), "");
}

void SettingsStore::set_session_memory(std::string_view session_id, std::string_view json_blob)
{
    set_string("mem." + std::string(session_id), json_blob);
}

void SettingsStore::delete_session_memory(std::string_view session_id)
{
    exec_prepared(
        *mutex_,
        *db_,
        "DELETE FROM kv_settings WHERE key = ?",
        "mem." + std::string(session_id));
}

std::string SettingsStore::get_messages_json(std::string_view session_id) const
{
    return get_string("msg." + std::string(session_id), "");
}

void SettingsStore::set_messages_json(std::string_view session_id, std::string_view json_blob)
{
    set_string("msg." + std::string(session_id), json_blob);
}

void SettingsStore::delete_messages_json(std::string_view session_id)
{
    exec_prepared(
        *mutex_,
        *db_,
        "DELETE FROM kv_settings WHERE key = ?",
        "msg." + std::string(session_id));
}

// ============================================================================
// v0.5.0: AgentImpl direct-wiring accessors
// ============================================================================

std::string SettingsStore::get_default_model_name() const
{
    return get_string("default_model_name", "");
}

void SettingsStore::set_default_model_name(std::string_view name)
{
    set_string("default_model_name", name);
}

std::string SettingsStore::get_current_provider() const
{
    return get_string("current_provider", "");
}

void SettingsStore::set_current_provider(std::string_view provider)
{
    set_string("current_provider", provider);
}

std::vector<std::string> SettingsStore::list_providers() const
{
    std::vector<std::string> out;
    auto rows = query_all_prepared(
        *mutex_,
        *db_,
        "SELECT name FROM providers ORDER BY name");
    out.reserve(rows.size());
    for (const auto& row : rows) {
        if (!row.empty() && !row[0].empty()) {
            out.push_back(row[0]);
        }
    }
    return out;
}

int SettingsStore::get_max_context_size_for_model(std::string_view model_name) const
{
    // 1. Check if the user has set an explicit max_context_size for this model.
    auto m = get_model(model_name);
    if (m.has_value() && m->max_context_size > 0) {
        return m->max_context_size;
    }

    // 2. §ProviderPlugins Phase 3: ask the provider's plugin — its static
    //    model table is the source of truth for its own models. An explicit
    //    `plugin` hint on the provider row wins; otherwise auto-detect from
    //    the provider name / host.
    if (m.has_value()) {
        if (auto p = get_provider(m->provider)) {
            const auto plugin = plugin_for_row(p->plugin, p->name, p->host);
            if (const auto n = plugin->context_size_for(m->model_id); n > 0) {
                return static_cast<int>(n);
            }
            if (const auto n = plugin->context_size_for(model_name); n > 0) {
                return static_cast<int>(n);
            }
        }
    }

    // 3. Fall back to the built-in default for known models (covers
    //    plugin-less providers: OpenAI / Anthropic / DeepSeek).
    int builtin = builtin_max_context_size(model_name);
    if (builtin > 0) return builtin;

    // 4. Ultimate fallback: 1M tokens (generous for unknown models).
    return 1000000;
}

int SettingsStore::get_max_agent_iterations() const
{
    return get_int("max_agent_iterations", 100);
}

size_t SettingsStore::get_max_tool_result_size() const
{
    return static_cast<size_t>(get_int("max_tool_result_size", 10000));
}

size_t SettingsStore::get_max_request_size() const
{
    return static_cast<size_t>(get_int("max_request_size", 4000000));
}

size_t SettingsStore::get_max_response_size() const
{
    return static_cast<size_t>(get_int("max_response_size", 5000000));
}

int SettingsStore::get_mcp_tool_schema_soft_cap_percent() const
{
    return get_int("mcp_tool_schema_soft_cap_percent", 5);
}

void SettingsStore::set_max_agent_iterations(int v)        { set_int("max_agent_iterations", v); }
void SettingsStore::set_max_tool_result_size(size_t v)    { set_int("max_tool_result_size", static_cast<int>(v)); }
void SettingsStore::set_max_request_size(size_t v)         { set_int("max_request_size", static_cast<int>(v)); }
void SettingsStore::set_max_response_size(size_t v)        { set_int("max_response_size", static_cast<int>(v)); }
void SettingsStore::set_mcp_tool_schema_soft_cap_percent(int v) { set_int("mcp_tool_schema_soft_cap_percent", v); }

ServerErrorSettings SettingsStore::get_server_error_settings() const
{
    ServerErrorSettings s;
    s.enabled = get_bool("server_error_enabled", s.enabled);
    s.initial_wait_seconds = get_int("server_error_initial_wait_seconds", s.initial_wait_seconds);
    s.max_wait_seconds = get_int("server_error_max_wait_seconds", s.max_wait_seconds);
    // §B24 fix: stod on user-provided string can throw — guard it.
    try {
        s.backoff_multiplier = std::stod(
            get_string("server_error_backoff_multiplier",
                       std::to_string(s.backoff_multiplier)));
    } catch (...) {
        // Leave default value in place — settings were corrupt / unparseable.
    }
    s.max_retries_per_call = get_int("server_error_max_retries_per_call", s.max_retries_per_call);
    return s;
}

// ============================================================================
// v0.5.0: MCP server registry
// ============================================================================
//
// These accessors store McpServer entries in the `mcp_servers` table.
// Hosts (or McpServerManager) call list_mcp_servers() on startup to
// load the user's configured MCP servers, replacing the old JSON file
// path. The one-shot JSON→SQLite migration was handled by the old CLI's
// --migrate-data flag (see src/tools/migrate.cpp).

namespace {
SettingsStore::McpServer row_to_mcp(const std::vector<std::string>& r)
{
    // SELECT order: name, transport, command, args_json, env_json, url,
    //               bearer_token, description, enabled
    SettingsStore::McpServer s;
    if (r.size() < 9) return s;
    s.name         = r[0];
    if (r.size() >= 10 && !r[9].empty()) s.origin = r[9];
    s.transport    = r[1].empty() ? "stdio" : r[1];
    s.command      = r[2];
    s.url          = r[5];
    s.bearer_token = r[6];
    s.description  = r[7];
    s.enabled      = (r[8] == "1" || r[8] == "true");
    if (!r[3].empty()) {
        try {
            auto j = nlohmann::json::parse(r[3]);
            if (j.is_array()) {
                for (const auto& a : j) s.args.push_back(a.get<std::string>());
            }
        } catch (...) { /* leave empty */ }
    }
    if (!r[4].empty()) {
        try {
            auto j = nlohmann::json::parse(r[4]);
            if (j.is_object()) {
                for (auto it = j.begin(); it != j.end(); ++it) {
                    s.env[it.key()] = it.value().get<std::string>();
                }
            }
        } catch (...) { /* leave empty */ }
    }
    return s;
}
}  // anonymous namespace

std::optional<SettingsStore::McpServer> SettingsStore::get_mcp_server(std::string_view name) const
{
    auto row = query_one_prepared(
        *mutex_,
        *db_,
        "SELECT name, transport, command, args_json, env_json, url, "
        "bearer_token, description, enabled, origin FROM mcp_servers WHERE name = ?",
        std::string(name));
    if (!row.has_value()) return std::nullopt;
    return row_to_mcp(*row);
}

std::vector<SettingsStore::McpServer> SettingsStore::list_mcp_servers() const
{
    std::vector<McpServer> out;
    auto rows = query_all_prepared(
        *mutex_,
        *db_,
        "SELECT name, transport, command, args_json, env_json, url, "
        "bearer_token, description, enabled, origin FROM mcp_servers ORDER BY name");
    out.reserve(rows.size());
    for (const auto& row : rows) {
        out.push_back(row_to_mcp(row));
    }
    return out;
}

void SettingsStore::upsert_mcp_server(const McpServer& s)
{
    nlohmann::json args_j = s.args;
    nlohmann::json env_j  = s.env;
    exec_prepared(
        *mutex_,
        *db_,
        "INSERT INTO mcp_servers(name, transport, command, args_json, env_json, "
        "url, bearer_token, description, enabled, origin) VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?, ?) "
        "ON CONFLICT(name) DO UPDATE SET "
        "transport=excluded.transport, command=excluded.command, "
        "args_json=excluded.args_json, env_json=excluded.env_json, "
        "url=excluded.url, bearer_token=excluded.bearer_token, "
        "description=excluded.description, enabled=excluded.enabled, "
        "origin=excluded.origin",
        s.name, s.transport, s.command,
        args_j.dump(), env_j.dump(),
        s.url, s.bearer_token, s.description,
        static_cast<int>(s.enabled), s.origin);
}

void SettingsStore::delete_mcp_server(std::string_view name)
{
    exec_prepared(
        *mutex_,
        *db_,
        "DELETE FROM mcp_servers WHERE name = ?",
        std::string(name));
}

bool SettingsStore::has_mcp_servers() const noexcept
{
    // §fix (2026-07-18): SQLite prepare/step can throw; this function is
    // noexcept, so an escaping exception would call std::terminate.
    try {
        std::lock_guard<std::recursive_mutex> lk(*mutex_);
        auto stmt = db_->prepare("SELECT 1 FROM mcp_servers LIMIT 1");
        return stmt.step();
    } catch (...) {
        return false;
    }
}

} // namespace hive
