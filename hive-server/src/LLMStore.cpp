// =============================================================================
// src/LLMStore.cpp — HiveMind-owned LLM provider & model configuration
// =============================================================================
#include "hivemind/DbUtil.h"
#include "hivemind/LLMStore.h"

#include <stdexcept>

namespace fs = std::filesystem;

namespace {
using hivemind::db::bind_text;
using hivemind::db::col_text;
inline void exec_sql(sqlite3* db, const char* sql) {
    hivemind::db::exec_sql(db, sql, "LLMStore");
}

constexpr const char* kSchema = R"sql(
CREATE TABLE IF NOT EXISTS providers (
    name               TEXT PRIMARY KEY,
    host               TEXT NOT NULL,
    api_key            TEXT NOT NULL,
    chat_endpoint      TEXT NOT NULL DEFAULT '/v1/chat/completions',
    discovery_endpoint TEXT NOT NULL DEFAULT '/v1/models',
    timeout_seconds    INTEGER NOT NULL DEFAULT 30,
    plugin             TEXT NOT NULL DEFAULT ''
);

CREATE TABLE IF NOT EXISTS models (
    name             TEXT PRIMARY KEY,
    provider         TEXT NOT NULL REFERENCES providers(name),
    model_id         TEXT NOT NULL,
    max_context_size INTEGER NOT NULL DEFAULT 0,
    temperature      REAL    NOT NULL DEFAULT 0.0,
    is_default       INTEGER NOT NULL DEFAULT 0
);
)sql";

struct Db {
    sqlite3* p = nullptr;
    explicit Db(const fs::path& path) {
        if (sqlite3_open(path.c_str(), &p) != SQLITE_OK) {
            std::string msg = p ? sqlite3_errmsg(p) : "out of memory";
            if (p) sqlite3_close(p);
            throw std::runtime_error(std::string("LLMStore: open failed: ") + msg);
        }
    }
    ~Db() { if (p) sqlite3_close(p); }
    sqlite3* get() const { return p; }
};

LlmProvider row_to_provider(sqlite3_stmt* st) {
    LlmProvider p;
    p.name               = col_text(st, 0);
    p.host               = col_text(st, 1);
    p.api_key            = col_text(st, 2);
    p.chat_endpoint      = col_text(st, 3);
    p.discovery_endpoint = col_text(st, 4);
    p.timeout_seconds    = sqlite3_column_int(st, 5);
    p.plugin             = col_text(st, 6);
    return p;
}

LlmModel row_to_model(sqlite3_stmt* st) {
    LlmModel m;
    m.name             = col_text(st, 0);
    m.provider         = col_text(st, 1);
    m.model_id         = col_text(st, 2);
    m.max_context_size = sqlite3_column_int(st, 3);
    m.temperature      = sqlite3_column_double(st, 4);
    m.is_default       = sqlite3_column_int(st, 5) != 0;
    return m;
}

constexpr const char* kProvCols =
    "SELECT name, host, api_key, chat_endpoint, discovery_endpoint,"
    "       timeout_seconds, plugin FROM providers";
constexpr const char* kModelCols =
    "SELECT name, provider, model_id, max_context_size, temperature, is_default"
    "  FROM models";
} // namespace

LLMStore::LLMStore() {
    db_path_ = fs::path{std::getenv("HOME") ? std::getenv("HOME") : "."}
             / ".local" / "share" / "hivemind" / "llm.db";
    fs::create_directories(db_path_.parent_path());
    init_schema();
    import_from_daedalus();
}

LLMStore::LLMStore(fs::path db) : db_path_(std::move(db)) {
    fs::create_directories(db_path_.parent_path());
    init_schema();
}

void LLMStore::init_schema() {
    Db db(db_path_);
    exec_sql(db.get(), kSchema);
}

// One-way import from daedalus' SettingsStore when our providers table is
// empty (first run after the Phase 13 migration). Read-only ATTACH.
void LLMStore::import_from_daedalus() {
    fs::path dae = fs::path{std::getenv("HOME") ? std::getenv("HOME") : "."}
                 / ".local" / "share" / "daedalus" / "daedalus.db";
    if (!fs::exists(dae)) return;

    Db db(db_path_);
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db.get(), "SELECT COUNT(*) FROM providers",
                           -1, &st, nullptr) != SQLITE_OK) return;
    int count = 0;
    if (sqlite3_step(st) == SQLITE_ROW) count = sqlite3_column_int(st, 0);
    sqlite3_finalize(st);
    if (count > 0) return;   // already populated — never re-import

    const std::string attach =
        "ATTACH DATABASE '" + dae.string() + "' AS dae";
    if (sqlite3_exec(db.get(), attach.c_str(), nullptr, nullptr, nullptr) != SQLITE_OK)
        return;
    exec_sql(db.get(),
        "INSERT INTO providers(name, host, api_key, chat_endpoint,"
        "                     discovery_endpoint, timeout_seconds, plugin)"
        " SELECT name, host, api_key, chat_endpoint, discovery_endpoint,"
        "        timeout_seconds, plugin FROM dae.providers");
    exec_sql(db.get(),
        "INSERT INTO models(name, provider, model_id, max_context_size,"
        "                  temperature, is_default)"
        " SELECT name, provider, model_id, max_context_size, temperature, 0"
        "   FROM dae.models");
    // Carry over daedalus' default-model choice.
    sqlite3_stmt* dst = nullptr;
    if (sqlite3_prepare_v2(db.get(),
            "SELECT value FROM dae.kv_settings WHERE key='default_model_name'",
            -1, &dst, nullptr) == SQLITE_OK) {
        if (sqlite3_step(dst) == SQLITE_ROW) {
            const std::string def = col_text(dst, 0);
            if (!def.empty()) {
                sqlite3_stmt* upd = nullptr;
                if (sqlite3_prepare_v2(db.get(),
                        "UPDATE models SET is_default=1 WHERE name=?",
                        -1, &upd, nullptr) == SQLITE_OK) {
                    bind_text(upd, 1, def);
                    sqlite3_step(upd);
                    sqlite3_finalize(upd);
                }
            }
        }
        sqlite3_finalize(dst);
    }
    sqlite3_exec(db.get(), "DETACH DATABASE dae", nullptr, nullptr, nullptr);
}

// ── providers ─────────────────────────────────────────────────────────
LlmProvider LLMStore::create_provider(const LlmProvider& p) {
    if (p.name.empty() || p.host.empty())
        throw std::invalid_argument("provider name and host are required");
    Db db(db_path_);
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db.get(),
            "INSERT INTO providers(name, host, api_key, chat_endpoint,"
            "                     discovery_endpoint, timeout_seconds, plugin)"
            " VALUES(?,?,?,?,?,?,?)", -1, &st, nullptr) != SQLITE_OK)
        throw std::runtime_error("LLMStore: prepare failed");
    bind_text(st, 1, p.name);
    bind_text(st, 2, p.host);
    bind_text(st, 3, p.api_key);
    bind_text(st, 4, p.chat_endpoint.empty() ? "/v1/chat/completions" : p.chat_endpoint);
    bind_text(st, 5, p.discovery_endpoint.empty() ? "/v1/models" : p.discovery_endpoint);
    sqlite3_bind_int(st, 6, p.timeout_seconds);
    bind_text(st, 7, p.plugin);
    const int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE)
        throw std::invalid_argument("provider '" + p.name + "' already exists");
    return *get_provider(p.name);
}

std::optional<LlmProvider> LLMStore::get_provider(const std::string& name) {
    Db db(db_path_);
    sqlite3_stmt* st = nullptr;
    const std::string sql = std::string(kProvCols) + " WHERE name=?";
    if (sqlite3_prepare_v2(db.get(), sql.c_str(), -1, &st, nullptr) != SQLITE_OK)
        return std::nullopt;
    bind_text(st, 1, name);
    std::optional<LlmProvider> out;
    if (sqlite3_step(st) == SQLITE_ROW) out = row_to_provider(st);
    sqlite3_finalize(st);
    return out;
}

std::vector<LlmProvider> LLMStore::list_providers() {
    Db db(db_path_);
    std::vector<LlmProvider> out;
    sqlite3_stmt* st = nullptr;
    const std::string sql = std::string(kProvCols) + " ORDER BY name";
    if (sqlite3_prepare_v2(db.get(), sql.c_str(), -1, &st, nullptr) != SQLITE_OK)
        return out;
    while (sqlite3_step(st) == SQLITE_ROW) out.push_back(row_to_provider(st));
    sqlite3_finalize(st);
    return out;
}

bool LLMStore::update_provider(const LlmProvider& p) {
    Db db(db_path_);
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db.get(),
            "UPDATE providers SET host=?, api_key=?, chat_endpoint=?,"
            " discovery_endpoint=?, timeout_seconds=?, plugin=?"
            " WHERE name=?", -1, &st, nullptr) != SQLITE_OK)
        return false;
    bind_text(st, 1, p.host);
    bind_text(st, 2, p.api_key);
    bind_text(st, 3, p.chat_endpoint);
    bind_text(st, 4, p.discovery_endpoint);
    sqlite3_bind_int(st, 5, p.timeout_seconds);
    bind_text(st, 6, p.plugin);
    bind_text(st, 7, p.name);
    const int rc = sqlite3_step(st);
    const int changed = sqlite3_changes(db.get());
    sqlite3_finalize(st);
    return rc == SQLITE_DONE && changed > 0;
}

bool LLMStore::delete_provider(const std::string& name) {
    Db db(db_path_);
    // Refuse while any model still references this provider.
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db.get(),
            "SELECT COUNT(*) FROM models WHERE provider=?",
            -1, &st, nullptr) != SQLITE_OK) return false;
    bind_text(st, 1, name);
    int refs = 0;
    if (sqlite3_step(st) == SQLITE_ROW) refs = sqlite3_column_int(st, 0);
    sqlite3_finalize(st);
    if (refs > 0) return false;

    if (sqlite3_prepare_v2(db.get(), "DELETE FROM providers WHERE name=?",
                           -1, &st, nullptr) != SQLITE_OK) return false;
    bind_text(st, 1, name);
    const int rc = sqlite3_step(st);
    const int changed = sqlite3_changes(db.get());
    sqlite3_finalize(st);
    return rc == SQLITE_DONE && changed > 0;
}

// ── models ────────────────────────────────────────────────────────────
LlmModel LLMStore::create_model(const LlmModel& m) {
    if (m.name.empty() || m.model_id.empty() || m.provider.empty())
        throw std::invalid_argument("model name, model_id and provider are required");
    if (!get_provider(m.provider))
        throw std::invalid_argument("unknown provider '" + m.provider + "'");
    Db db(db_path_);
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db.get(),
            "INSERT INTO models(name, provider, model_id, max_context_size,"
            "                  temperature, is_default) VALUES(?,?,?,?,?,?)",
            -1, &st, nullptr) != SQLITE_OK)
        throw std::runtime_error("LLMStore: prepare failed");
    bind_text(st, 1, m.name);
    bind_text(st, 2, m.provider);
    bind_text(st, 3, m.model_id);
    sqlite3_bind_int(st, 4, m.max_context_size);
    sqlite3_bind_double(st, 5, m.temperature);
    sqlite3_bind_int(st, 6, m.is_default ? 1 : 0);
    const int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE)
        throw std::invalid_argument("model '" + m.name + "' already exists");
    if (m.is_default) set_default_model(m.name);  // ensure single default
    return *get_model(m.name);
}

std::optional<LlmModel> LLMStore::get_model(const std::string& name) {
    Db db(db_path_);
    sqlite3_stmt* st = nullptr;
    const std::string sql = std::string(kModelCols) + " WHERE name=?";
    if (sqlite3_prepare_v2(db.get(), sql.c_str(), -1, &st, nullptr) != SQLITE_OK)
        return std::nullopt;
    bind_text(st, 1, name);
    std::optional<LlmModel> out;
    if (sqlite3_step(st) == SQLITE_ROW) out = row_to_model(st);
    sqlite3_finalize(st);
    return out;
}

std::vector<LlmModel> LLMStore::list_models() {
    Db db(db_path_);
    std::vector<LlmModel> out;
    sqlite3_stmt* st = nullptr;
    const std::string sql = std::string(kModelCols) + " ORDER BY name";
    if (sqlite3_prepare_v2(db.get(), sql.c_str(), -1, &st, nullptr) != SQLITE_OK)
        return out;
    while (sqlite3_step(st) == SQLITE_ROW) out.push_back(row_to_model(st));
    sqlite3_finalize(st);
    return out;
}

bool LLMStore::update_model(const LlmModel& m) {
    Db db(db_path_);
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db.get(),
            "UPDATE models SET provider=?, model_id=?, max_context_size=?,"
            " temperature=? WHERE name=?", -1, &st, nullptr) != SQLITE_OK)
        return false;
    bind_text(st, 1, m.provider);
    bind_text(st, 2, m.model_id);
    sqlite3_bind_int(st, 3, m.max_context_size);
    sqlite3_bind_double(st, 4, m.temperature);
    bind_text(st, 5, m.name);
    const int rc = sqlite3_step(st);
    const int changed = sqlite3_changes(db.get());
    sqlite3_finalize(st);
    return rc == SQLITE_DONE && changed > 0;
}

bool LLMStore::delete_model(const std::string& name) {
    Db db(db_path_);
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db.get(), "DELETE FROM models WHERE name=?",
                           -1, &st, nullptr) != SQLITE_OK) return false;
    bind_text(st, 1, name);
    const int rc = sqlite3_step(st);
    const int changed = sqlite3_changes(db.get());
    sqlite3_finalize(st);
    return rc == SQLITE_DONE && changed > 0;
}

bool LLMStore::set_default_model(const std::string& name) {
    if (!get_model(name)) return false;
    Db db(db_path_);
    exec_sql(db.get(), "UPDATE models SET is_default=0");
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db.get(),
            "UPDATE models SET is_default=1 WHERE name=?",
            -1, &st, nullptr) != SQLITE_OK) return false;
    bind_text(st, 1, name);
    const int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    return rc == SQLITE_DONE;
}

std::optional<LlmModel> LLMStore::default_model() {
    Db db(db_path_);
    sqlite3_stmt* st = nullptr;
    const std::string sql = std::string(kModelCols) + " WHERE is_default=1";
    if (sqlite3_prepare_v2(db.get(), sql.c_str(), -1, &st, nullptr) != SQLITE_OK)
        return std::nullopt;
    std::optional<LlmModel> out;
    if (sqlite3_step(st) == SQLITE_ROW) out = row_to_model(st);
    sqlite3_finalize(st);
    return out;
}

std::optional<ResolvedLlmModel> LLMStore::resolve(const std::string& name) {
    auto m = name.empty() ? default_model() : get_model(name);
    if (!m) return std::nullopt;
    auto p = get_provider(m->provider);
    if (!p || p->host.empty()) return std::nullopt;
    ResolvedLlmModel out;
    out.name             = m->name;
    out.model_id         = m->model_id;
    out.provider         = p->name;
    out.host             = p->host;
    out.api_key          = p->api_key;
    out.chat_endpoint    = p->chat_endpoint;
    out.max_context_size = m->max_context_size;
    out.temperature      = m->temperature;
    return out;
}
