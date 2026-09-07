/**
 * @file hive-brain/default_paths.h
 * @brief Default filesystem locations matching the historical CLI layout.
 *
 * === Per-project data (the only tier most users care about) ===
 *
 * Each Agent instance working on a project gets its own per-project
 * namespace, namespaced by `agent_name`:
 *
 *     agent_name == ""    ->  <project_root>/.daedalus/hive-brain/
 *     agent_name == "bob" ->  <project_root>/.daedalus/daedalus-bob/
 *
 * The empty string is the backward-compat default. Multiple Agent
 * instances on the same project each get their own subdir so
 * memories never leak between agents on the same project.
 *
 * The legacy directory names `.daedalus/` and `.daedalus-<name>/`
 * are still honoured as a read fallback for one release cycle so
 * existing projects keep working. NEW writes always go to the
 * new `.daedalus/hive-brain*` path.
 *
 * === Per-user (global) data ===
 *
 * Settings (models, MCP servers, kv pairs) live in the SQLite
 * SettingsStore at `$XDG_DATA_HOME/hive-brain/hive-agent.db`.
 * Use `hive::SettingsStore::instance()` to access it.
 */
#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace hive {

/// $HOME/.daedalus (or $HIVE_AGENT_HOME, or the XDG-compliant path)
std::filesystem::path default_agent_home();

/// @brief Validate an agent_name string (§Phase 8.8).
///
/// Returns true if the name matches `^[A-Za-z0-9_.-]{1,64}$` AND
/// does not start with '.' or '-' (which would respectively
/// denote hidden directories or CLI flags). Empty string is
/// permitted — it means "no namespace, use the project default".
///
/// Rationale for each reject case lives in Agent.md §Phase 8.8.
[[nodiscard]] bool is_valid_agent_name(std::string_view name) noexcept;

/// @brief Resolve the per-agent runtime-state directory (§Phase 8.8).
///
/// Returns `<project_root>/.daedalus/hive-brain/` (empty agent_name) or
/// `<project_root>/.daedalus/daedalus-<name>/` (named agent_name).
///
/// Legacy paths (`<project_root>/.daedalus/` and
/// `<project_root>/.daedalus-<name>/`) are honoured as a read fallback
/// for one release cycle; use ProjectPaths::active_agent_dir() when
/// loading existing data.
///
/// The directory is NOT auto-created — callers do that on first write.
[[nodiscard]] std::filesystem::path resolve_agent_dir(
    const std::filesystem::path& project_root,
    std::string_view agent_name);

}  // namespace hive
