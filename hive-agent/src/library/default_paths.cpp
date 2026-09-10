/**
 * @file default_paths.cpp
 * @brief Thin forwarders to `Paths::global()` (Plan.md Phase 1).
 *
 * As of Plan.md Phase 1, every `default_*` helper in this file is a
 * thin inline forwarder into `Paths::global()`. The legacy
 * `~/.daedalus/` location is honoured via `Paths::user_root()`'s
 * built-in probe (see paths.cpp for the algorithm). The behaviour
 * change vs. the pre-Phase-1 implementation is:
 *
 *   - The user-tier overrides are now `$HIVE_AGENT_HOME`,
 *     `$HIVE_AGENT_CONFIG_HOME`, etc. (the legacy `$DAEDALUS_HOME`
 *     is NO LONGER honoured at this layer; use `$HIVE_AGENT_HOME`
 *     instead, matching the public API in paths.h).
 *   - When `$XDG_*` variables are set, the helpers honour them
 *     (previously they did not — see CHANGELOG.md "Plan.md Phase 1").
 *
 * Per-project helpers (`resolve_agent_dir`, `is_valid_agent_name`)
 * remain here for backward compat with code that doesn't want to
 * instantiate a `ProjectPaths`. New code should use `ProjectPaths`.
 */
#include "library/default_paths.h"

#include "library/paths.h"
#include "library/project_paths.h"

#include <regex>

namespace hive {

std::filesystem::path default_agent_home()
{
	return Paths::global().user_root();
}

// v0.4.0: deprecated — settings moved to SQLite SettingsStore.
// Retained for `daedalus-cli --migrate-data` (which still imports
// legacy files) and as a one-release-cycle read-fallback helper.
[[deprecated("v0.4.0: settings moved to SQLite (SettingsStore); "
             "use SettingsStore::instance() instead. Will be "
             "removed in v0.6.0.")]]
std::filesystem::path default_settings_path()
{
	return Paths::global().config_file();
}

[[deprecated("v0.4.0: MCP servers moved to SQLite (SettingsStore); "
             "use list_mcp_servers() / upsert_mcp_server() "
             "instead. Will be removed in v0.6.0.")]]
std::filesystem::path default_mcp_servers_path()
{
	return Paths::global().mcp_servers_file();
}

[[deprecated("v0.4.0: MCP env moved to SettingsStore::mcp_servers.env; "
             "use list_mcp_servers() instead. Will be removed in "
             "v0.6.0.")]]
std::filesystem::path default_mcp_env_path()
{
	return Paths::global().mcp_env_file();
}

bool is_valid_agent_name( std::string_view name ) noexcept
{
	if ( name.empty() ) return true;
	if ( name[0] == '.' || name[0] == '-' ) return false;
	if ( name.size() > 64 ) return false;
	static const std::regex re( "^[A-Za-z0-9_.-]+$" );
	return std::regex_match( std::string( name ), re );
}

std::filesystem::path resolve_agent_dir(
	const std::filesystem::path& project_root,
	std::string_view agent_name )
{
	// Same algorithm as ProjectPaths::agent_dir() — we delegate so
	// all per-project path resolution lives in one place.
	ProjectPaths pp( project_root, std::string( agent_name ) );
	return pp.agent_dir();
}

}  // namespace hive