#include "McpServerManager.h"
#include "HttpMcpTransport.h"
#include "SchemaSanitize.h"
#include <fstream>
#include <iostream>
#include <sstream>
#include <algorithm>
#include <cstdlib>
#include <pwd.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#include <filesystem>
#include <mutex>
#include <thread>

#include "core/Logger.h"
#include "core/settings.h"   // §v0.5.0: load_from_store() reads from here

// §Phase 5.1: helper macro that converts LOG_CERR(INFO,  ...); style
// statements into LOG_INFO/LOG_WARNING/LOG_ERROR calls via the
// existing Logger. Same formatted text routed through the Logger
// instead of writing to stderr directly.
#define LOG_CERR(level, expr) do {                          \
        std::ostringstream _hive_msg;                   \
        _hive_msg << expr;                               \
        LOG_##level(_hive_msg.str());                    \
    } while (0)
namespace mcp {

// =============================================================================
// Breaker tuning constants (§9.7.B).
//
// Cooldown grows exponentially per consecutive failure: 5s → 10s → 20s → 40s
// → 80s → 160s → 300s (capped). Any successful call resets fail_count and
// broken_until to defaults.
// =============================================================================
static constexpr int kBreakerBaseCooldownSec   = 5;
static constexpr int kBreakerMaxCooldownSec    = 300;

// Helpers — header-only, file-local.
static int breaker_cooldown_for(int fail_count) {
    int cd = kBreakerBaseCooldownSec;
    for (int i = 1; i < fail_count && cd < kBreakerMaxCooldownSec; ++i) {
        cd *= 2;
    }
    if (cd > kBreakerMaxCooldownSec) cd = kBreakerMaxCooldownSec;
    return cd;
}

static bool breaker_is_open_now(const McpServerManager::Breaker& b,
                                std::chrono::steady_clock::time_point now) {
    return b.broken_until.time_since_epoch().count() != 0 && now < b.broken_until;
}

// (s_instance removed in Phase 2 — no static singleton state.)

// Get the default instance. Both instance() and delete_default() route
// through the same static pointer, so reset-after-delete is safe.
namespace {
    McpServerManager*& default_mcp_ref() {
        static McpServerManager* g_default = nullptr;
        return g_default;
    }
}

McpServerManager& McpServerManager::instance() {
    auto& p = default_mcp_ref();
    if (!p) {
        p = new McpServerManager();
    }
    return *p;
}

void McpServerManager::delete_default() {
    auto& p = default_mcp_ref();
    if (p) {
        p->disconnect_all();
        delete p;
        p = nullptr;
    }
}

McpServerManager::~McpServerManager() {
    disconnect_all();
}

std::string McpServerManager::get_env_file_path() const {
    // Always use the XDG-compliant path. The .env file contains global
    // env vars (e.g. MINIMAX_API_KEY) needed by MCP servers at runtime.
    const char* xdg = std::getenv("XDG_CONFIG_HOME");
    const char* home = std::getenv("HOME");
    if (!home) {
        struct passwd* pw = getpwuid(getuid());
        if (pw) home = pw->pw_dir;
    }
    if (xdg && *xdg) return std::string(xdg) + "/hive-brain/mcp/.env";
    if (home && *home) return std::string(home) + "/.config/hive-brain/mcp/.env";
    return "";
}

bool McpServerManager::load_env_file(const std::string& env_path) {
    env_vars_.clear();

    // §Plan.md v0.5.0: derive default path when not supplied.
    std::string actual_path = env_path.empty() ? get_env_file_path() : env_path;

    std::ifstream file(actual_path);
    if (!file.is_open()) {
        return false;  // .env file is optional
    }

    std::string line;
    while (std::getline(file, line)) {
        // Skip comments and empty lines
        if (line.empty() || line[0] == '#') continue;

        // Find the = sign
        size_t pos = line.find('=');
        if (pos != std::string::npos) {
            std::string key = line.substr(0, pos);
            std::string value = line.substr(pos + 1);

            // Trim whitespace
            auto trim = [](std::string& s) {
                s.erase(0, s.find_first_not_of(" \t"));
                s.erase(s.find_last_not_of(" \t") + 1);
            };
            trim(key);
            trim(value);

            // Remove quotes if present
            if (value.size() >= 2) {
                if ((value.front() == '"' && value.back() == '"') ||
                    (value.front() == '\'' && value.back() == '\'')) {
                    value = value.substr(1, value.size() - 2);
                }
            }

            env_vars_[key] = value;
        }
    }

    return true;
}

std::string McpServerManager::substitute_env_vars(const std::string& input) {
    std::string result = input;
    size_t pos = 0;

    while ((pos = result.find("${", pos)) != std::string::npos) {
        size_t end = result.find('}', pos);
        if (end == std::string::npos) break;

        std::string var_name = result.substr(pos + 2, end - pos - 2);

        // Check loaded env vars first, then system environment
        std::string value;
        auto it = env_vars_.find(var_name);
        if (it != env_vars_.end()) {
            value = it->second;
        } else {
            const char* env_val = getenv(var_name.c_str());
            if (env_val) {
                value = env_val;
            }
        }

        result.replace(pos, end - pos + 1, value);
        pos += value.length();
    }

    return result;
}

std::map<std::string, std::string> McpServerManager::prepare_env(const McpServerConfig& config) {
    std::map<std::string, std::string> env;

    // Copy current environment
    for (char** e = environ; *e; ++e) {
        std::string var(*e);
        size_t pos = var.find('=');
        if (pos != std::string::npos) {
            env[var.substr(0, pos)] = var.substr(pos + 1);
        }
    }

    // Add/substitute from config
    for (const auto& [key, value] : config.env) {
        env[key] = substitute_env_vars(value);
    }

    return env;
}

std::string McpServerManager::build_command_string(const McpServerConfig& config) {
    std::ostringstream cmd;
    cmd << config.command;
    for (const auto& arg : config.args) {
        cmd << " \"" << arg << "\"";
    }
    return cmd.str();
}
// §v0.5.0: Concrete loader called from the templated public API. Touches
// hive::SettingsStore directly. Returning through void* keeps the
// SettingsStore header out of McpServerManager.h.

// §v0.5.0: Concrete loader called from the templated public API. Touches
// hive::SettingsStore directly. Returning through void* keeps the
// SettingsStore header out of McpServerManager.h.
bool McpServerManager::load_from_store_impl(void* store_ptr)
{
    auto& store = *static_cast<hive::SettingsStore*>(store_ptr);

    // load_from_store() is exclusive — it RESETS the configs_ map before
    // loading. This matches the test contract in test_mcp_load_from_store.cpp
    // (ML5: after disabling a server, calling load_from_store() again removes
    // the disabled entry from the visible set — merge semantics would leave
    // the stale entry in the map).
    configs_.clear();
    config_loaded_ = false;
    clients_.clear();
    http_transports_.clear();
    server_tools_.clear();
    mark_mcp_tools_dirty();

    auto servers = store.list_mcp_servers();
    for (const auto& s : servers) {
        if (is_server_disabled(s.name)) {
            LOG_CERR(INFO, "[MCP] skipping disabled server '" << s.name << "'\n");
            continue;
        }
        // §2026-09-30: honor the db row's own enabled flag. The check above
        // only honors the config-file disabled list — a server disabled via
        // mcp_disable (or directly in the store) reconnected on every brain
        // restart, resurrecting whatever made it need disabling (Bob's
        // schema-poisoned comfy server re-muted him after a container
        // restart despite enabled=0 in his db).
        if (!s.enabled) {
            LOG_CERR(INFO, "[MCP] skipping store-disabled server '"
                     << s.name << "' (enabled=0)\n");
            continue;
        }
        McpServerConfig sc;
        sc.name         = s.name;
        sc.command      = s.command;
        sc.args         = s.args;
        sc.env          = s.env;
        sc.transport    = s.transport;
        sc.url          = s.url;
        sc.bearer_token = s.bearer_token;
        sc.description  = s.description;
        sc.enabled      = s.enabled;
        if (sc.is_valid()) {
            configs_[s.name] = sc;
        } else {
            LOG_CERR(INFO,  "[MCP] WARNING: rejected server entry '"
                      << s.name << "' from store — ");
            if (sc.is_http()) {
                LOG_CERR(INFO,  "transport is http but 'url' is missing or empty\n");
            } else {
                LOG_CERR(INFO,  "stdio requires non-empty 'command' and 'args'\n");
            }
        }
    }

    config_loaded_ = true;
    return true;
}

// Explicit instantiation for the only concrete type we support now.
template bool McpServerManager::load_from_store<hive::SettingsStore>(
    hive::SettingsStore& store);

bool McpServerManager::connect(const std::string& server_name) {
    last_error_.clear();

    // Already connected on either transport?
    if (clients_.find(server_name) != clients_.end()) return true;
    if (http_transports_.find(server_name) != http_transports_.end()) return true;

    // §9.7.B — Don't try to connect if the breaker is open. Caller can
    // use force_reconnect() to override.
    {
        std::scoped_lock lk(breakers_mu_);
        auto bit = breakers_.find(server_name);
        if (bit != breakers_.end() &&
            breaker_is_open_now(bit->second, std::chrono::steady_clock::now())) {
            last_error_ = "Server '" + server_name +
                          "' is in circuit-breaker cooldown";
            return false;
        }
    }

    // Find configuration
    auto config_it = configs_.find(server_name);
    if (config_it == configs_.end()) {
        last_error_ = "Server '" + server_name + "' not found in configuration";
        return false;
    }

    const McpServerConfig& config = config_it->second;

    // ------------------------------------------------------------------
    // HTTP transport branch
    // ------------------------------------------------------------------
    if (config.is_http()) {
        try {
            auto transport = std::make_unique<HttpMcpTransport>(
                config.url,
                config.bearer_token);

            auto connect_result = transport->connect();
            if (!connect_result.has_value()) {
                transport.reset();
                last_error_ = "Failed to connect (HTTP) to '" + server_name +
                              "': " + to_string(connect_result.error());
                breaker_record_failure(server_name);
                return false;
            }

            if (!http_initialize(transport.get())) {
                transport->disconnect();
                // last_error_ set by http_initialize
                breaker_record_failure(server_name);
                return false;
            }

            // Cache the tool list so get_tools(name) works without re-fetching.
            server_tools_[server_name] = http_list_tools(transport.get());

            http_transports_[server_name] = std::move(transport);
            mark_mcp_tools_dirty();
            breaker_record_success(server_name);
            return true;
        } catch (const std::exception& e) {
            // §9.7.B — any exception during connect() is one bad server;
            // do NOT propagate.
            last_error_ = std::string("HTTP connect threw: ") + e.what();
            breaker_record_failure(server_name);
            return false;
        } catch (...) {
            last_error_ = "HTTP connect threw unknown exception";
            breaker_record_failure(server_name);
            return false;
        }
    }

    // ------------------------------------------------------------------
    // stdio transport branch (existing behaviour, unchanged)
    // ------------------------------------------------------------------
    try {
        auto client = std::make_unique<McpClient>();

        // Build command string and prepare environment
        std::string cmd = build_command_string(config);
        std::map<std::string, std::string> env = prepare_env(config);

        // Connect
        if (!client->connect(cmd, env)) {
            last_error_ = "Failed to connect to '" + server_name + "': " +
                          client->get_last_error();
            breaker_record_failure(server_name);
            return false;
        }

        // Initialize
        if (!client->initialize()) {
            last_error_ = "Failed to initialize '" + server_name + "': " +
                          client->get_last_error();
            client->disconnect();
            breaker_record_failure(server_name);
            return false;
        }

        // List tools and cache them
        server_tools_[server_name] = client->list_tools();

        // §9.7.C — register the list_changed callback. The dispatch is
        // INLINE inside McpClient::read_response(), so there is no
        // separate thread to manage. The callback fires when an
        // interleaved notifications/tools/list_changed frame is seen
        // mid-read; the caller (this client) keeps waiting for the
        // response to its actual request. Crucially, this replaces the
        // previous separate-listener-thread design which raced with the
        // main reader for `read()` on the shared stdout pipe and could
        // eat response bytes — see McpClient.h §9.7.C for the rationale.
        client->set_list_changed_callback([this, server_name]() {
            this->handle_list_changed(server_name);
        });
        set_list_changed_supported(server_name, true);

        // Store client
        clients_[server_name] = std::move(client);

        mark_mcp_tools_dirty();
        breaker_record_success(server_name);
        // §Phase39-H1: recovery must be VISIBLE — breaker failures log,
        // so successes (first connect or post-cooldown retry) do too.
        LOG_CERR(INFO, "[MCP] server '" << server_name << "' connected — "
                 << server_tools_[server_name].size() << " tool(s)");
        return true;
    } catch (const std::exception& e) {
        last_error_ = std::string("stdio connect threw: ") + e.what();
        breaker_record_failure(server_name);
        return false;
    } catch (...) {
        last_error_ = "stdio connect threw unknown exception";
        breaker_record_failure(server_name);
        return false;
    }
}

void McpServerManager::disconnect(const std::string& server_name) {
    auto cit = clients_.find(server_name);
    if (cit != clients_.end()) {
        cit->second->disconnect();
        clients_.erase(cit);
    }
    auto hit = http_transports_.find(server_name);
    if (hit != http_transports_.end()) {
        hit->second->disconnect();
        http_transports_.erase(hit);
    }
    server_tools_.erase(server_name);
    mark_mcp_tools_dirty();
}

void McpServerManager::disconnect_all() {
    // §9.7.A — full teardown path. Idempotent.
    shutdown();
}

bool McpServerManager::is_connected(const std::string& server_name) const {
    auto cit = clients_.find(server_name);
    if (cit != clients_.end() && cit->second->is_connected()) return true;
    auto hit = http_transports_.find(server_name);
    if (hit != http_transports_.end() && hit->second->is_connected()) return true;
    return false;
}

// =============================================================================
// Hub public API implementations (§9.6/§9.7).
// =============================================================================

// §9.7.A — Process lifecycle.
// Tears down every client/transport: for stdio, McpClient::disconnect()
// sends SIGTERM to the server's OWN process group (children call
// setpgid(0, 0) in McpClient::connect), waits up to 2s per child, then
// escalates to SIGKILL. After this returns, no stdio MCP subprocess
// should be alive.
void McpServerManager::shutdown() {
    // §fix (2026-07-18): dropped the function-static std::once_flag —
    // it was shared across ALL manager instances, so the first manager
    // to shut down consumed it and every later manager's shutdown()
    // became a no-op, leaking its MCP child processes. The body is
    // already idempotent (disconnect() + clear()).
    for (auto& [name, client] : clients_) {
        client->disconnect();
    }
    for (auto& [name, transport] : http_transports_) {
        transport->disconnect();
    }
    clients_.clear();
    http_transports_.clear();
    server_tools_.clear();
    breakers_.clear();
    list_changed_supported_.clear();
    mark_mcp_tools_dirty();
}

McpServerManager::ToolIndex McpServerManager::aggregate_capabilities() {
    ToolIndex idx;
    // stdio — only the McpClient* itself carries the connection.
    for (auto& [name, client] : clients_) {
        if (!client->is_connected()) continue;
        for (const auto& def : server_tools_[name]) {
            idx[{name, def.name}] = client.get();
        }
    }
    // HTTP transports don't have a McpClient* (the transport IS the
    // transport). Use a sentinel: the address of the entry in the
    // http_transports_ map, cast through void*. Tools routed via HTTP
    // are dispatched by call_tool() itself, not via this index; this
    // index is informational. Document this in the field's role.
    //
    // For now we leave HTTP entries out of the routing index; the
    // dispatch is done by call_tool() which dispatches on the underlying
    // transport kind.
    return idx;
}

bool McpServerManager::is_healthy(const std::string& server) const {
    if (!is_connected(server)) return false;
    std::scoped_lock lk(breakers_mu_);
    auto it = breakers_.find(server);
    if (it == breakers_.end()) return true;
    return !breaker_is_open_now(it->second, std::chrono::steady_clock::now());
}

McpClient* McpServerManager::resolve(const std::string& server,
                                     const std::string& /*tool*/) {
    if (!is_healthy(server)) return nullptr;
    auto cit = clients_.find(server);
    if (cit != clients_.end() && cit->second->is_connected()) {
        return cit->second.get();
    }
    // HTTP path: routing is handled inside call_tool(); there is no
    // McpClient* to return. Caller should fall back to call_tool().
    return nullptr;
}

bool McpServerManager::force_reconnect(const std::string& server) {
    disconnect(server);
    {
        std::scoped_lock lk(breakers_mu_);
        breakers_.erase(server);
    }
    return connect(server);
}

McpServerManager::Breaker McpServerManager::breaker_for(const std::string& server) const {
    std::scoped_lock lk(breakers_mu_);
    auto it = breakers_.find(server);
    if (it == breakers_.end()) return Breaker{};
    return it->second;
}

// §9.7.C — Token-window management.
// `ToolFactory::refresh_mcp_tools()` sets `schema_dumped_` indirectly by
// writing through this method after the proxied surface is rebuilt. The
// Manager only owns the sticky cache.
size_t McpServerManager::estimated_token_count() const {
    std::scoped_lock lk(schema_mu_);
    // ≈ 1 token per 4 characters (GPT-style heuristic, conservative).
    return schema_dumped_.size() / 4;
}

void McpServerManager::handle_list_changed(const std::string& server) {
    // Cap rebuilds at 1/s per server with a simple timestamp check; coalesce
    // rapid notifications.
    auto& supported = list_changed_supported_[server];
    if (!supported) return;   // Server never opted into list_changed.
    auto now = std::chrono::steady_clock::now();
    {
        std::scoped_lock lk(breakers_mu_);
        auto& b = breakers_[server];
        if (b.last_failure.time_since_epoch().count() != 0 &&
            std::chrono::duration_cast<std::chrono::seconds>(
                now - b.last_failure).count() < 1) {
            return;  // Within the 1s debounce window.
        }
        b.last_failure = now;
    }
    // Re-fetch this server's tool list, then mark dirty.
    auto cit = clients_.find(server);
    auto hit = http_transports_.find(server);
    try {
        if (cit != clients_.end() && cit->second->is_connected()) {
            server_tools_[server] = cit->second->list_tools();
        } else if (hit != http_transports_.end() &&
                   hit->second->is_connected()) {
            server_tools_[server] = http_list_tools(hit->second.get());
        } else {
            return;  // Not connected — let the next refresh handle it.
        }
    } catch (...) {
        // List-fetch failure: bump breaker, do not crash.
        std::scoped_lock lk(breakers_mu_);
        auto& b = breakers_[server];
        ++b.fail_count;
        b.broken_until = std::chrono::steady_clock::now() +
                        std::chrono::seconds(breaker_cooldown_for(b.fail_count));
        return;
    }
    mark_mcp_tools_dirty();
}

void McpServerManager::set_list_changed_supported(const std::string& server,
                                                 bool supported) {
    list_changed_supported_[server] = supported;
}

// Breaker mutators used by connect()/call_tool(). Made private member
// methods (was anonymous-namespace) so the call sites can refer to them
// no matter where in the file they appear.
void McpServerManager::breaker_record_success(const std::string& server) {
    std::scoped_lock lk(breakers_mu_);
    auto& b = breakers_[server];
    b.fail_count = 0;
    b.broken_until = {};
    b.last_failure = {};
}

void McpServerManager::breaker_record_failure(const std::string& server) {
    std::scoped_lock lk(breakers_mu_);
    auto& b = breakers_[server];
    ++b.fail_count;
    b.last_failure = std::chrono::steady_clock::now();
    const int cd = breaker_cooldown_for(b.fail_count);
    b.broken_until = b.last_failure + std::chrono::seconds(cd);
    LOG_CERR(INFO, "[MCP] server '" << server
              << "' failed (count=" << b.fail_count
              << "), retrying in " << cd << "s"
              << (last_error_.empty() ? "" : " — " + last_error_));
}

McpClient* McpServerManager::get_client(const std::string& server_name) {
    auto it = clients_.find(server_name);
    if (it == clients_.end()) {
        return nullptr;
    }
    return it->second.get();
}

const McpServerConfig* McpServerManager::get_config(const std::string& server_name) const {
    auto it = configs_.find(server_name);
    if (it == configs_.end()) {
        return nullptr;
    }
    return &it->second;
}

void McpServerManager::add_config(const McpServerConfig& config) {
    if (config.is_valid()) {
        configs_[config.name] = config;
        config_loaded_ = true;
    }
}

bool McpServerManager::enable_server(const McpServerConfig& config) {
    add_config(config);
    disabled_servers_.erase(config.name);
    mark_mcp_tools_dirty();
    if (is_connected(config.name)) {
        return true;
    }
    return connect(config.name);
}

void McpServerManager::disable_server(const std::string& name) {
    disabled_servers_.insert(name);
    disconnect(name);
    mark_mcp_tools_dirty();
}

std::vector<std::string> McpServerManager::list_configured_servers() const {
    std::vector<std::string> servers;
    for (const auto& [name, config] : configs_) {
        servers.push_back(name);
    }
    return servers;
}

std::vector<std::string> McpServerManager::list_connected_servers() const {
    std::vector<std::string> servers;
    for (const auto& [name, client] : clients_) {
        // §Phase 15: skip disabled servers even if a stale connection exists.
        if (is_server_disabled(name)) continue;
        if (client->is_connected()) {
            servers.push_back(name);
        }
    }
    for (const auto& [name, transport] : http_transports_) {
        if (is_server_disabled(name)) continue;
        if (transport->is_connected()) {
            servers.push_back(name);
        }
    }
    return servers;
}

std::vector<ToolDefinition> McpServerManager::get_all_tools() {
    std::vector<ToolDefinition> all_tools;

    for (const auto& [server_name, tools] : server_tools_) {
        // §Phase 15: disabled server's tools don't leak to the agent.
        if (is_server_disabled(server_name)) continue;
        for (const auto& tool : tools) {
            all_tools.push_back(tool);
        }
    }

    return all_tools;
}

std::vector<ToolDefinition> McpServerManager::get_tools(const std::string& server_name) {
    auto it = server_tools_.find(server_name);
    if (it != server_tools_.end()) {
        return it->second;
    }
    return {};
}

// ================ Dirty-flag hooks (Agent.md §18) ================

void McpServerManager::mark_mcp_tools_dirty() noexcept {
    mcp_tools_dirty_.store(true, std::memory_order_release);
}

bool McpServerManager::mcp_tools_dirty() const noexcept {
    return mcp_tools_dirty_.load(std::memory_order_acquire);
}

void McpServerManager::clear_mcp_tools_dirty() noexcept {
    mcp_tools_dirty_.store(false, std::memory_order_release);
}

// =============================================================================

ToolResult McpServerManager::call_tool(const std::string& server_name,
                                      const std::string& tool_name,
                                      const json& arguments) {
    // §9.7.B — if the breaker is open for this server, return a
    // server-aware offline message instead of wasting a call.
    {
        std::scoped_lock lk(breakers_mu_);
        auto bit = breakers_.find(server_name);
        if (bit != breakers_.end() &&
            breaker_is_open_now(bit->second, std::chrono::steady_clock::now())) {
            int retry = breaker_cooldown_for(bit->second.fail_count);
            return {false, "",
                    server_name + "__" + tool_name +
                    " is currently offline (will retry after " +
                    std::to_string(retry) + "s)"};
        }
    }

    // HTTP transport
    auto hit = http_transports_.find(server_name);
    if (hit != http_transports_.end()) {
        try {
            ToolResult r = http_call_tool(hit->second.get(), tool_name, arguments);
            if (r.success) {
                breaker_record_success(server_name);
            } else {
                breaker_record_failure(server_name);
            }
            return r;
        } catch (...) {
            breaker_record_failure(server_name);
            return {false, "", "HTTP call threw on '" + server_name + "'"};
        }
    }

    // stdio transport
    McpClient* client = get_client(server_name);
    if (!client) {
        return {false, "", "Server '" + server_name + "' not connected"};
    }

    try {
        ToolResult r = client->call_tool(tool_name, arguments);
        if (r.success) {
            breaker_record_success(server_name);
        } else {
            breaker_record_failure(server_name);
        }
        return r;
    } catch (...) {
        breaker_record_failure(server_name);
        return {false, "", "stdio call threw on '" + server_name + "'"};
    }
}

// ============================================================================
// HTTP backend helpers
// ============================================================================

bool McpServerManager::http_initialize(McpTransport* transport) {
    json params = {
        {"protocolVersion", "2025-06-18"},
        {"capabilities", {{"roots", json::object()}, {"sampling", json::object()}}},
        {"clientInfo", {{"name", "hive-brain"}, {"version", "1.0.0"}}}
    };

    auto fut = transport->send_request("initialize", params, 10000);
    auto status = fut.wait_for(std::chrono::seconds(15));
    if (status != std::future_status::ready) {
        last_error_ = "HTTP initialize: timeout waiting for response";
        return false;
    }
    auto result = fut.get();
    if (!result.has_value()) {
        last_error_ = std::string("HTTP initialize: ") + to_string(result.error());
        return false;
    }

    // HTTP transport returns the result body (not the full envelope).
    if (!result.value().is_object()) {
        last_error_ = "HTTP initialize: unexpected response shape";
        return false;
    }

    // Send initialized notification (no response expected).
    auto notif = transport->send_notification("notifications/initialized", json::object());
    if (!notif.has_value()) {
        // Not fatal - notification failures are advisory.
        LOG_CERR(INFO, "[MCP] WARNING: HTTP 'initialized' notification failed: "
                  << to_string(notif.error()));
    }

    return true;
}

std::vector<ToolDefinition> McpServerManager::http_list_tools(McpTransport* transport) {
    auto fut = transport->send_request("tools/list", json::object(), 30000);
    auto status = fut.wait_for(std::chrono::seconds(35));
    if (status != std::future_status::ready) {
        last_error_ = "HTTP tools/list: timeout";
        return {};
    }
    auto result = fut.get();
    if (!result.has_value()) {
        last_error_ = std::string("HTTP tools/list: ") + to_string(result.error());
        return {};
    }

    std::vector<ToolDefinition> tools;
    if (result.value().is_object() && result.value().contains("tools")) {
        for (const auto& tool : result.value()["tools"]) {
            ToolDefinition def;
            def.name        = tool.value("name", "");
            def.description = tool.value("description", "");
            def.input_schema = tool.value("inputSchema", json::object());
            // §2026-09-30: one poisoned schema (recursive/over-deep shape)
            // 400s EVERY provider request — sanitize at ingestion so a bad
            // server can never mute the agent (4th schema incident).
            if (sanitize_tool_schema(def.input_schema))
                LOG_CERR(WARNING, "[MCP] tool '" << def.name
                         << "': inputSchema sanitized for provider"
                            " compatibility (recursive/over-deep shape)\n");
            tools.push_back(def);
        }
    }
    return tools;
}

ToolResult McpServerManager::http_call_tool(McpTransport* transport,
                                            const std::string& tool_name,
                                            const json& arguments) {
    json params = {
        {"name", tool_name},
        {"arguments", arguments}
    };

    auto fut = transport->send_request("tools/call", params, 30000);
    auto status = fut.wait_for(std::chrono::seconds(35));
    if (status != std::future_status::ready) {
        return {false, "", std::string("HTTP tools/call: timeout")};
    }
    auto result = fut.get();
    if (!result.has_value()) {
        return {false, "", std::string("HTTP tools/call: ") + to_string(result.error())};
    }

    // Result body has shape { content: [{type: "text", text: "..."}], isError?: bool }
    const json& body = result.value();
    std::string content;
    if (body.is_object() && body.contains("content") && body["content"].is_array()) {
        for (const auto& item : body["content"]) {
            if (item.is_object() && item.contains("text")) {
                content += item["text"].get<std::string>();
            }
        }
    }
    return {true, content, std::nullopt};
}

} // namespace mcp
