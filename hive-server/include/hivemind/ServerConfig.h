#pragma once

#include <filesystem>
#include <string>
#include <vector>

/**
 * ServerConfig — server-level settings (HiveMind 2.0, workspace config).
 *
 * Backed by a tiny JSON file (~/.local/share/hivemind/server.json) —
 * human-editable, no schema migrations. Currently holds:
 *
 *   workspace_root — HOST folder bind-mounted at /workspace in every agent
 *                    container ("the company share"). Default:
 *                    ~/.local/share/hivemind/workspace
 *
 * Changing workspace_root affects containers created AFTER the change;
 * existing containers keep their mounts until removed.
 */
class ServerConfig {
public:
    static ServerConfig& instance();          // app singleton (default path)
    explicit ServerConfig(std::filesystem::path file);  // tests / custom

    /// Absolute, tilde-expanded workspace root (created if missing).
    std::string workspace_root();

    /// Validate + persist a new root. Throws std::invalid_argument if the
    /// path is not absolute (after ~ expansion) or cannot be created.
    void set_workspace_root(const std::string& path);

    /// Phase 9: CORS allowlist. Empty (default) = no CORS headers — the
    /// same-origin web UI keeps working; browsers block cross-origin fetches.
    /// Entries are exact origins ("https://hive.example.com") or "*".
    std::vector<std::string> cors_origins() const { return cors_origins_; }
    void set_cors_origins(std::vector<std::string> origins);
    bool cors_allowed(const std::string& origin) const;

    /// Phase 28: Xvfb geometry for agent screens ("1280x800" default).
    /// Applies to screen stacks STARTED after the change.
    std::string screen_resolution() const { return screen_resolution_; }
    // Phase 30.2: dedicated WebSocket port for the hive-worker channel
    // (OmniNet's WebSocket::Server wraps its own HTTP server, so it
    // cannot share the main API port).
    int worker_ws_port() const { return worker_ws_port_; }
    // Phase 33: loopback MCP bridge (hive-brain streamable-http → worker channel)
    const std::string& worker_ws_bind() const { return worker_ws_bind_; }
    void set_screen_resolution(const std::string& res);   // validates WxH

    /// Phase 55: default window of messages returned by GET
    /// /api/threads/:id/messages when no ?after/?before cursor is given
    /// (initial thread load). Clients page back with ?before=<id>.
    int message_window() const { return message_window_; }
    void set_message_window(int n);   // validates 10..5000

    /// Phase 28: pre-start every agent's container at boot so the first
    /// turn isn't a cold start. Default true.
    bool warm_containers() const { return warm_containers_; }
    void set_warm_containers(bool on);

    /// Expand a leading "~/" to $HOME. Static + pure for tests.
    static std::string expand_home(const std::string& path);

    /// Default root when nothing is configured.
    static std::string default_workspace_root();
    static void seed_workspace(const std::filesystem::path& root);

private:
    void load();
    void save() const;

    std::filesystem::path file_;
    std::string workspace_root_;
    std::vector<std::string> cors_origins_;   // "" = use default
    bool warm_containers_ = true;
    std::string screen_resolution_ = "1280x800";
    int message_window_ = 200;
    int worker_ws_port_ = 8423;
    std::string worker_ws_bind_ = "0.0.0.0";   // "127.0.0.1" behind nginx
};
