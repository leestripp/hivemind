#pragma once

#include <filesystem>
#include <string>

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

    /// Expand a leading "~/" to $HOME. Static + pure for tests.
    static std::string expand_home(const std::string& path);

    /// Default root when nothing is configured.
    static std::string default_workspace_root();

private:
    void load();
    void save() const;

    std::filesystem::path file_;
    std::string workspace_root_;   // "" = use default
};
