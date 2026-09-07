#pragma once

#include "McpServerConfig.h"
#include "McpClient.h"
#include "McpTransport.h"
#include <atomic>
#include <chrono>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace mcp {

// Manages multiple MCP server connections.
// Servers are loaded from the SQLite SettingsStore via load_from_store().
// Two transport kinds are supported per server:er:
//   - stdio  : spawn a subprocess, talk JSON-RPC over its stdin/stdout (via McpClient).
//   - http   : POST JSON-RPC to a URL (via McpTransport/HTTP implementation).
//
// Servers are routed to the appropriate backend at connect() time based on the
// server's `transport` field (defaulting to stdio for back-compat).
class McpServerManager {
public:
    // Phase 2: public constructor. No longer a true singleton.
    // Production code (Agent) constructs and owns its own instance.
    McpServerManager() = default;

    // Legacy singleton accessor. Returns a process-wide default instance
    // that is created lazily on first call. Production code should
    // construct its own McpServerManager instead.
    static McpServerManager& instance();

    // Drop the process-wide default. Tests use this to reset state.
    static void delete_default();

    // Prevent copying
    McpServerManager(const McpServerManager&) = delete;
    McpServerManager& operator=(const McpServerManager&) = delete;

    // §Phase 15: per-server gate. Set of server names that the host has
    // disabled. Empty = all enabled. load_from_store() skips disabled servers.
    void set_disabled_servers(const std::set<std::string>& names) { disabled_servers_ = names; }
    bool is_server_disabled(const std::string& name) const { return disabled_servers_.count(name) > 0; }
    [[nodiscard]] std::set<std::string> get_disabled_servers() const { return disabled_servers_; }

    // §Phase 16: runtime enable/disable of a single server.
    // enable_server() adds/updates the in-memory config and attempts to
    // connect; disable_server() disconnects and marks tools dirty.
    bool enable_server(const McpServerConfig& config);
    void disable_server(const std::string& name);
    void add_config(const McpServerConfig& config);

    // §v0.5.0: Load configuration from a SettingsStore (SQLite).
    // Pops McpServer entries out of the daedalus.db's mcp_servers table
    // and ingests them into the same in-memory configs_ map that
    // load_config() uses. Use this when the user has migrated to the
    // SQLite-backed settings.
    //
    // This is declared in the header as a free function template so the
    // SettingsStore header doesn't pollute the McpServerManager public API.
    // See load_from_store_impl() in McpServerManager.cpp for the impl.
    template <typename StoreT>
    bool load_from_store(StoreT& store) {
        return load_from_store_impl(&store);
    }

private:
    // §v0.5.0: Concrete loader. Implementation is in McpServerManager.cpp.
    // Public API is the templated load_from_store() above; this is the
    // single place that actually touches hive::SettingsStore.
    bool load_from_store_impl(void* store_ptr);

public:
    // Connect to a specific server by name.
    // Dispatches to McpClient (stdio) or McpTransport (HTTP) based on the
    // server's configured transport.
    // Returns true on success.
    bool connect(const std::string& server_name);

    // Disconnect from a server (works for either transport kind).
    void disconnect(const std::string& server_name);

    // Disconnect from all servers (both transports).
    void disconnect_all();

    // Check if connected to a server.
    // Returns true if the named server has an active connection on EITHER
    // transport kind.
    bool is_connected(const std::string& server_name) const;

    // Get stdio client for a specific server.
    // Returns nullptr if not connected, or if the server uses HTTP transport.
    McpClient* get_client(const std::string& server_name);

    // Get configuration for a server (any transport kind).
    const McpServerConfig* get_config(const std::string& server_name) const;

    // List all configured servers (from config, regardless of transport)
    std::vector<std::string> list_configured_servers() const;

    // List all connected servers (both transport kinds)
    std::vector<std::string> list_connected_servers() const;

    // Get all available tools from all connected servers (both transports)
    std::vector<ToolDefinition> get_all_tools();

    // Get tools from a specific server (either transport)
    std::vector<ToolDefinition> get_tools(const std::string& server_name);

    // Call a tool on a specific server.
    // Dispatches to McpClient::call_tool (stdio) or the HTTP transport's
    // JSON-RPC path. Returns a uniformly-shaped ToolResult.
    ToolResult call_tool(const std::string& server_name,
                        const std::string& tool_name,
                        const json& arguments);

    // Get last error message
    std::string get_last_error() const { return last_error_; }

    // Check if configuration is loaded
    bool is_config_loaded() const { return config_loaded_; }

    // Get path to .env file for loading environment variables
    std::string get_env_file_path() const;

    // §FIX-MCP-STORE-LOAD (2026-07-17): explicit reset.
    // load_from_store() MERGE into configs_; call clear() before a
    // sequence of loads if you want exclusive semantics. AgentImpl does
    // this on Agent init.
    void clear()
    {
        configs_.clear();
        config_loaded_ = false;
        clients_.clear();
        http_transports_.clear();
        server_tools_.clear();
        mark_mcp_tools_dirty();
    }

    // -------------------------------------------------------------------
    // Hub public API (mcp-standard §9.6/§9.7).
    //
    // The agent side talks only to this surface; the transport details
    // (stdio vs HTTP, child PID bookkeeping, retries) are hidden behind it.
    // -------------------------------------------------------------------

    // §9.7.A — Process lifecycle. Idempotent; safe to call multiple times.
    void shutdown();

    // §9.6.A — Initialisation & aggregation.
    using ToolIndex = std::map<std::pair<std::string,std::string>,
                                McpClient*>;
    ToolIndex aggregate_capabilities();

    // §9.7.B — Error isolation / health. A server is "healthy" only when
    // it has a live connection AND the circuit breaker is not currently
    // open for it.
    bool is_healthy(const std::string& server) const;

    // §9.6.C — Execution routing. Returns nullptr if the (server, tool)
    // pair is unknown OR the server's breaker is currently open.
    McpClient* resolve(const std::string& server, const std::string& tool);

    // §9.7.B — Force a fresh reconnect for one server, clearing its
    // breaker. Used by the settings-reload path and by tests.
    bool force_reconnect(const std::string& server);

    // §9.7.B — Per-server circuit breaker state.
    struct Breaker {
        int fail_count = 0;
        std::chrono::steady_clock::time_point broken_until{};
        std::chrono::steady_clock::time_point last_failure{};
    };
    Breaker breaker_for(const std::string& server) const;

    // §9.7.C — Token-window management.
    // Sticky, conservative estimate of the schema size in tokens (≈ chars/4).
    // Returns 0 when the surface has never been refreshed.
    size_t estimated_token_count() const;

    // §9.7.C — Notifications / list_changed.
    // Called by the per-server listener thread when the server emits
    // notifications/tools/list_changed. Re-fetches that server's tool list
    // and marks the manager dirty so the next refresh_mcp_tools() rebuilds
    // the McpProxiedTool surface.
    void handle_list_changed(const std::string& server);

    // Records whether the server declared `listChanged` capability. Called
    // after a successful initialize() during connect().
    void set_list_changed_supported(const std::string& server, bool supported);

    // -------------------------------------------------------------------
    // Test accessors. Use sparingly — only from tests/. The breaker mutex
    // lets a test hold the lock across multiple assertions without races.
    // -------------------------------------------------------------------
    std::mutex& breakers_mu_for_test() { return breakers_mu_; }
    std::map<std::string, Breaker>& breakers_for_test() { return breakers_; }
    void set_schema_dumped_for_test(std::string s) {
        std::scoped_lock lk(schema_mu_);
        schema_dumped_ = std::move(s);
    }

    // -------------------------------------------------------------------
    // Dirty-flag hooks for ToolFactory::refresh_mcp_tools() integration.
    //
    // Agent.md §18: the tool surface is rebuilt lazily. Callers
    // (LLMClient, a refresh button, etc.) observe `mcp_tools_dirty()`
    // and trigger ToolFactory::refresh_mcp_tools() before the next LLM
    // turn. We set the flag from inside connect()/disconnect()/
    // load_config() so callers don't need to subscribe to per-event
    // callbacks.
    // -------------------------------------------------------------------

    /** Mark the MCP tool surface as needing a refresh. */
    void mark_mcp_tools_dirty() noexcept;

    /** Query whether a refresh is pending. */
    bool mcp_tools_dirty() const noexcept;

    /** Clear the dirty flag (call after refresh_mcp_tools() succeeds). */
    void clear_mcp_tools_dirty() noexcept;

    // §Phase 11: public destructor — per-Agent instances are owned by
    // unique_ptr<AgentImpl::Body>::mcp which calls delete via the public
    // destructor. The old global-pattern singleton kept it private and
    // required delete_default(); that restriction is lifted here.
public:
    ~McpServerManager();

    // Load environment variables from .env file (needed by AgentImpl)
    bool load_env_file(const std::string& env_path = "");

private:

    // Substitute ${VAR_NAME} with environment variable values
    std::string substitute_env_vars(const std::string& input);

    // Convert env map to the format expected by McpClient::connect
    std::map<std::string, std::string> prepare_env(const McpServerConfig& config);

    // Build command string from args
    std::string build_command_string(const McpServerConfig& config);

    // HTTP backend helpers. Each is the HTTP equivalent of the corresponding
    // McpClient method, operating on a McpTransport*.
    bool http_initialize(McpTransport* transport);
    std::vector<ToolDefinition> http_list_tools(McpTransport* transport);
    ToolResult http_call_tool(McpTransport* transport,
                              const std::string& tool_name,
                              const json& arguments);

    // §9.7.B — per-server circuit breaker helpers. Mutate `breakers_`
    // under `breakers_mu_`. These used to live in an anonymous namespace
    // in McpServerManager.cpp, but that broke compile order — putting
    // them here keeps the call sites (`connect()`, `call_tool()`) simple.
    void breaker_record_success(const std::string& server);
    void breaker_record_failure(const std::string& server);

    // (s_instance removed in Phase 2 — no static singleton state.)

    // Configuration for all servers (any transport kind)
    std::map<std::string, McpServerConfig> configs_;
    std::set<std::string>                 disabled_servers_;  // §Phase 15

    // Active stdio client connections
    std::map<std::string, std::unique_ptr<McpClient>> clients_;

    // Active HTTP transport connections (streamable-HTTP via HttpMcpTransport)
    std::map<std::string, std::unique_ptr<McpTransport>> http_transports_;

    // Cached tools for each server (both transports)
    std::map<std::string, std::vector<ToolDefinition>> server_tools_;

    // Loaded environment variables from .env
    std::map<std::string, std::string> env_vars_;

    // Last error message
    std::string last_error_;

    // Whether config has been loaded
    bool config_loaded_ = false;

    // Set by connect()/disconnect()/load_from_store() to signal that the
    // ToolFactory should rebuild its McpProxiedTool surface.
    std::atomic<bool> mcp_tools_dirty_{true};

    // Per-server circuit breaker state (§9.7.B). Guarded by `breakers_mu_`.
    mutable std::mutex breakers_mu_;
    std::map<std::string, Breaker> breakers_;

    // Per-server `listChanged` capability flag (§9.7.C).
    std::map<std::string, bool> list_changed_supported_;

    // Sticky snapshot of the JSON-encoded capability surface, used by
    // estimated_token_count(). Rebuilt by refresh_mcp_tools().
    mutable std::mutex schema_mu_;
    std::string schema_dumped_;
};

} // namespace mcp