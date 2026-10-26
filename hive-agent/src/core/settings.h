// SPDX-License-Identifier: BSL-1.0
// include/hive-brain/settings.h
//
// Public, opaque settings API for the engine.
//
// What this replaces (v0.3.0+):
//   - `~/.config/hive-brain/config.json`        → `models` + `kv_settings`
//   - `~/.config/hive-brain/mcp/servers.json`    → `mcp_servers`
//   - `~/.config/hive-brain/mcp/.env`            → `kv_settings`
//
// What this DOES NOT touch:
//   - Skills subsystem (file-based by design — editable, version-controllable)
//   - Per-project session memory / messages — those move into the DB
//     separately in v0.3.5.
//
// Backed by SQLite at `<XDG_DATA_HOME>/hive-brain/hive-agent.db`
// (default `~/.local/share/hive-brain/hive-agent.db`).
//
// Thread-safety: all methods are safe to call from multiple threads.
// The underlying SQLite handle is opened in serialized mode.

#pragma once

#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// §B2 fix (v0.5.1): OmniNet::DB::Database is now referenced directly in
// the class body (no pimpl), so the full type must be visible here.
#include <db/Database.h>

#include "types/ProviderConfig.h"


namespace hive {

/// Server error retry/backoff configuration. Mirrors
/// settings.json::server_error.
struct ServerErrorSettings
{
    bool enabled = true;
    int initial_wait_seconds = 30;
    int max_wait_seconds = 300;
    double backoff_multiplier = 2.0;
    int max_retries_per_call = 10;
};

/// Opaque handle to the settings database.
///
/// Obtained via:
///   - `SettingsStore::instance()`     — global default, one DB per user
///   - `SettingsStore::for_home(path)` — per-Agent DB under `path`
///
/// The handle is cheap to copy. All copies share the same underlying
/// SQLite connection (singleton-per-path via DbPool). The dtor does NOT
/// close the DB — the process keeps one connection per unique path.
///
/// §B2 fix (v0.5.1): previously `for_home()` returned `SettingsStore&`
/// via a leaked `*(new SettingsStore)`. Now it returns by value, so the
/// wrapper is destroyed by the caller's RAII container. The underlying
/// sqlite3 handle lives in DbPool as before.
class SettingsStore
{
public:
    /// Global default. Singleton. First call opens the DB lazily.
    /// Returns by reference because the underlying static is a true
    /// singleton — the address is stable for the process lifetime.
    static SettingsStore& instance();

    /// Per-agent settings root. Pass the same `home` and you get a handle
    /// that shares the same DbPool connection as any prior call with the
    /// same `home`. Pass a different `home` and you get a distinct DB under
    /// `<home>/hive-agent.db`.
    ///
    /// §B2 fix: returns by value (move-constructed from the internal
    /// temporary). The DbPool entry stays alive for the process duration;
    /// only the lightweight wrapper is destroyed when the returned value
    /// goes out of scope.
    static SettingsStore for_home(std::filesystem::path home);

    // -- Read --
    std::optional<ProviderConfig> get_provider(std::string_view name) const;
    std::vector<std::string>      list_providers() const;
    std::vector<ProviderConfig>   list_providers_detailed() const;

    std::optional<ModelConfig> get_model(std::string_view name) const;
    std::vector<ModelConfig>   list_models() const;

    /// Effective settings for a model: model-specific values merged with
    /// the referenced provider's host/api_key/endpoints.
    std::optional<ResolvedModelConfig> resolve_model(std::string_view name) const;

    std::string get_string(std::string_view key, std::string_view def = "") const;
    bool        get_bool  (std::string_view key, bool              def = false) const;
    int         get_int   (std::string_view key, int               def = 0)     const;

    // -- Write --
    void upsert_provider(const ProviderConfig& p);
    void delete_provider(std::string_view name);

    void upsert_model (const ModelConfig& m);
    void delete_model (std::string_view name);

    void set_string(std::string_view key, std::string_view value);
    void set_bool  (std::string_view key, bool              value);
    void set_int   (std::string_view key, int               value);

    // -- Atomic --
    void transaction(std::function<void(SettingsStore&)> fn);

    // -- Introspection --
    std::filesystem::path db_path() const noexcept;

    /// True if this store has any user data (models OR non-schema kv rows).
    /// Used by subsystems to decide whether the SQLite store is
    /// authoritative for v0.4.0 (and JSON can be ignored).
    bool has_data() const noexcept;

    // -----------------------------------------------------------------------
    // v0.5.0: AgentImpl wires directly to SettingsStore ().
    // These are the remaining accessors needed by AgentImpl that weren't
    // in the initial v0.4.0 API.
    // -----------------------------------------------------------------------

    /// Get the default model name from kv_settings. Empty if none set.
    std::string get_default_model_name() const;

    /// Set the default model name in kv_settings.
    void set_default_model_name(std::string_view name);

    /// Get the current provider label from kv_settings. Empty if none set.
    std::string get_current_provider() const;

    /// Set the current provider label in kv_settings.
    void set_current_provider(std::string_view provider);

    /// Get the resolved max_context_size for `model_name`.
    /// Returns the user's setting from `models` table if set (non-zero),
    /// otherwise falls back to the built-in default for known models.
    /// Returns 1000000 (1M) as the ultimate fallback for unknown models.
    int get_max_context_size_for_model(std::string_view model_name) const;

    // -----------------------------------------------------------------------
    // v0.5.0: remaining agent settings that LLMClient needs.
    // .
    // Defaults: max_iterations=100, max_tool_result=10KB, max_request=4MB,
    //           max_response=5MB.
    // -----------------------------------------------------------------------

    /// Get max_agent_iterations from kv_settings. Default 100.
    int get_max_agent_iterations() const;

    /// Get max_tool_result_size from kv_settings. Default 10000 (10KB).
    size_t get_max_tool_result_size() const;

    /// Get max_request_size from kv_settings. Default 4000000 (4MB).
    size_t get_max_request_size() const;

    /// Get max_response_size from kv_settings. Default 5000000 (5MB).
    size_t get_max_response_size() const;

    // -----------------------------------------------------------------------
    // v0.5.0: MCP tool schema soft-cap percent (used by LLMClient).
    // Default 5% — §18.9.C.
    // -----------------------------------------------------------------------

    /// Get mcp.tool_schema_soft_cap_percent from kv_settings. Default 5.
    int get_mcp_tool_schema_soft_cap_percent() const;

    // -----------------------------------------------------------------------
    // v0.5.0: setters for the values the agent can change at runtime.
    // (Reads exist for LLMClient; writes exist for /models + tests.)
    // -----------------------------------------------------------------------

    void set_max_agent_iterations(int v);
    void set_max_tool_result_size(size_t v);
    void set_max_request_size(size_t v);
    void set_max_response_size(size_t v);
    void set_mcp_tool_schema_soft_cap_percent(int v);

    // -----------------------------------------------------------------------
    // v0.5.0: server error retry settings.
    // -----------------------------------------------------------------------

    /// Get server_error settings from kv_settings. Returns defaults if none set.
    ServerErrorSettings get_server_error_settings() const;

    // -----------------------------------------------------------------------
    // v0.3.5: per-project persistence (session memory + messages).
    //
    // For v0.3.5 these are write-through mirrors of the existing JSON
    // files at <data_dir>/memory/memory.json and <data_dir>/messages.json.
    // The data is stored in the same SQLite DB under namespaced keys:
    //
    //   "mem.<session_id>"        → JSON-encoded SessionMemory payload
    //   "msg.<session_id>"        → JSON-encoded messages array
    //
    // v0.4.0 swaps to DB-primary and deletes the JSON files.
    // ---------------------------------------------------------------------

    /// Get the persisted session-memory JSON for `session_id`. Empty if none.
    std::string get_session_memory(std::string_view session_id) const;

    /// Persist the session-memory JSON. Stores under "mem.<session_id>".
    void set_session_memory(std::string_view session_id, std::string_view json_blob);

    /// Drop the persisted session-memory for `session_id`.
    void delete_session_memory(std::string_view session_id);

    /// Get the persisted messages JSON for `session_id`. Empty if none.
    std::string get_messages_json(std::string_view session_id) const;

    /// Persist the messages JSON. Stores under "msg.<session_id>".
    void set_messages_json(std::string_view session_id, std::string_view json_blob);

    /// Drop the persisted messages for `session_id`.
    void delete_messages_json(std::string_view session_id);

    // -----------------------------------------------------------------------
    // v0.5.0: MCP server registry. The `mcp_servers` table replaces the
    // legacy `servers.json` file. Populated via upsert_mcp_server() or
    // daedalus-cli --migrate-data.
    // ----------------------------------------------------------------------

    /// One MCP server config. Mirrors the McpServerConfig struct but lives
    /// in the public API so hosts can read/write the registry directly.
    struct McpServer
    {
        std::string name;
        std::string transport = "stdio";        // stdio | streamable-http
        std::string command;                    // stdio: executable
        std::vector<std::string> args;          // stdio: argv
        std::map<std::string, std::string> env; // stdio: env vars
        std::string url;                        // http: endpoint
        std::string bearer_token;               // http: Authorization header
        std::string description;
        bool enabled = true;
        // Phase 71: 'hub' = roster-seeded (hub-authoritative, read-only
        // to the agent's mcp_* tools); 'local' = agent self-service.
        std::string origin = "local";
    };

    /// Get one MCP server by name.
    std::optional<McpServer> get_mcp_server(std::string_view name) const;

    /// List all configured MCP servers.
    std::vector<McpServer> list_mcp_servers() const;

    /// Insert or replace one MCP server.
    void upsert_mcp_server(const McpServer& s);

    /// Delete one MCP server by name.
    void delete_mcp_server(std::string_view name);

    /// True if any MCP servers are registered.
    bool has_mcp_servers() const noexcept;

private:
    // §B2 fix (v0.5.1): Impl fields are now inlined directly into the class
    // (not pimpl'd). This makes SettingsStore copyable/movable so
    // for_home() can return by value without leaking.
    //
    // db_ is non-owning — the actual sqlite3 handle is owned by DbPool
    // (singleton-per-path). Multiple SettingsStore instances can share the
    // same db_ pointer safely because SQLite runs in serialized mode.
    //
    // §webui-model-edit: a shared recursive mutex is used to serialise all
    // prepared-statement access. Prepared statements do not acquire OmniNet's
    // internal database mutex, so without this two threads could step on the
    // same sqlite3 handle concurrently and silently lose writes.
    std::filesystem::path path_;
    OmniNet::DB::Database* db_ = nullptr;
    mutable std::shared_ptr<std::recursive_mutex> mutex_;

    /// Internal ctor. `auto_migrate` is accepted for source compatibility
    /// but has no effect — auto-migration was removed in v0.5.2.
    SettingsStore(std::filesystem::path db_path, bool auto_migrate = false);
};

} // namespace hive
