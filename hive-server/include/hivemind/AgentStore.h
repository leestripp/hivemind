#pragma once

#include <optional>
#include <string>
#include <vector>
#include <filesystem>
#include <sqlite3.h>

/**
 * AgentStore — SQLite persistence for the agent roster (HiveMind 2.0).
 *
 * A RosterAgent is a persistent, named teammate (see Plan.md Phase 1).
 * Runtime state (daedalus messages/memory) lives on disk under
 * ~/.local/share/hivemind/agents/<id>/ — this DB row is the registry.
 *
 * Thread-safety: same pattern as GraphStore — fresh connection per op.
 * DB file: ~/.local/share/hivemind/agents.db (created on first access).
 */
struct RosterAgent {
    std::string id;
    std::string name;           // unique, daedalus-compatible agent name
    std::string role;           // persona / job description ("" = general)
    std::string model;          // daedalus SettingsStore model name ("" = store default)
    std::string workspace_mode; // "shared" (company /workspace) — containers land in Phase 2
    std::string container_id;   // "" until Phase 2
    std::string created_at;
    std::string updated_at;
};

class AgentStore {
public:
    AgentStore();                                  // default XDG path
    explicit AgentStore(std::filesystem::path db); // tests / custom path
    ~AgentStore();

    /// Create an agent. Throws std::invalid_argument on empty/duplicate name.
    RosterAgent create_agent(const std::string& name,
                             const std::string& role = "",
                             const std::string& model = "",
                             const std::string& workspace_mode = "shared");

    std::optional<RosterAgent> get_agent(const std::string& id);
    std::optional<RosterAgent> get_agent_by_name(const std::string& name);
    std::vector<RosterAgent> list_agents();

    /// Update role/model/name/workspace_mode. Throws std::invalid_argument
    /// if the agent does not exist or the new name clashes.
    RosterAgent update_agent(const std::string& id,
                             const std::string& name,
                             const std::string& role,
                             const std::string& model,
                             const std::string& workspace_mode);

    /// Delete the roster row (on-disk agent dir is left untouched).
    bool delete_agent(const std::string& id);

    /// Base directory holding per-agent runtime dirs.
    static std::filesystem::path agents_base_dir();

private:
    std::filesystem::path db_path_;
    void ensure_schema();
    sqlite3* open_conn();
};
