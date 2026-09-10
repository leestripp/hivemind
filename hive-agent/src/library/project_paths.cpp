/**
 * @file project_paths.cpp
 * @brief Per-project, per-agent filesystem location resolver.
 *
 * Resolves `<project_root>/.hive-agent/<name-or-default>/` for every
 * (project_root, agent_name) pair. The same ProjectPaths logic
 * is shared with the legacy `resolve_agent_dir()` helper in
 * `src/library/default_paths.cpp` so all per-project path
 * resolution lives in one place.
 */
#include "library/project_paths.h"

#include "library/default_paths.h"  // is_valid_agent_name()

#include <system_error>

namespace hive {

namespace fs = std::filesystem;

namespace {
constexpr std::string_view kAgentBaseDir        = ".hive-agent";
constexpr std::string_view kAgentDirNewPrefix   = "default";
constexpr std::string_view kAgentDirNewNamedPrefix = "";   // .hive-agent/<name>
// Phase 43 renames (daedalus → hive-agent): TWO old layouts migrate.
constexpr std::string_view kAgentDirPrevBase    = ".daedalus";
constexpr std::string_view kAgentDirPrevPrefix  = "hive-brain";
constexpr std::string_view kAgentDirPrevNamedPrefix = "daedalus-";
constexpr std::string_view kAgentDirLegacyPrefix = ".daedalus";
constexpr std::string_view kAgentDirLegacyNamedPrefix = ".daedalus-";

fs::path resolve_agent_dir_impl(
	const fs::path& project_root,
	std::string_view agent_name )
{
	// New layout: <project>/.hive-agent/default/  (default)
	//             <project>/.hive-agent/<name>/    (named)
	fs::path inner = agent_name.empty()
	                     ? std::string( kAgentDirNewPrefix )
	                     : ( std::string( kAgentDirNewNamedPrefix ) + std::string( agent_name ) );
	return project_root / std::string( kAgentBaseDir ) / inner;
}

fs::path resolve_legacy_agent_dir_impl(
	const fs::path& project_root,
	std::string_view agent_name )
{
	// Legacy layout: <project>/.daedalus/        (default)
	//                <project>/.daedalus-<name>/ (named)
	return agent_name.empty()
	           ? project_root / std::string( kAgentDirLegacyPrefix )
	           : project_root / ( std::string( kAgentDirLegacyNamedPrefix ) + std::string( agent_name ) );
}
}  // namespace

ProjectPaths::ProjectPaths( fs::path project_root, std::string agent_name )
	: m_project_root( std::move( project_root ) )
	, m_agent_name( std::move( agent_name ) )
{
	if ( !is_valid_agent_name( m_agent_name ) ) {
		throw std::invalid_argument(
			"ProjectPaths: invalid agent_name (see is_valid_agent_name rules)" );
	}
}

const fs::path& ProjectPaths::project_root() const noexcept
{
	return m_project_root;
}

std::string_view ProjectPaths::agent_name() const noexcept
{
	return m_agent_name;
}

const fs::path& ProjectPaths::agent_dir() const noexcept
{
	if ( !m_agent_dir_resolved ) {
		m_agent_dir_cache = resolve_agent_dir_impl( m_project_root, m_agent_name );
		m_agent_dir_resolved = true;
	}
	return m_agent_dir_cache;
}

fs::path ProjectPaths::legacy_agent_dir() const
{
	return resolve_legacy_agent_dir_impl( m_project_root, m_agent_name );
}

fs::path ProjectPaths::active_agent_dir() const
{
	std::error_code ec;
	fs::path current = agent_dir();
	if ( fs::exists( current, ec ) ) {
		return current;
	}
	// Phase 43: migrate the previous layout (.daedalus/daedalus-<name> or
	// .daedalus/hive-brain) in place — rename keeps every byte of agent
	// state (memory, todos, logs, sessions) without a copy.
	fs::path prev = m_project_root / std::string( kAgentDirPrevBase )
					/ ( m_agent_name.empty()
						? std::string( kAgentDirPrevPrefix )
						: ( std::string( kAgentDirPrevNamedPrefix ) + m_agent_name ) );
	if ( fs::is_directory( prev, ec ) ) {
		fs::create_directories( current.parent_path(), ec );
		if ( fs::rename( prev, current, ec ), !ec ) return current;
		ec.clear();
		return prev;   // rename failed (cross-fs?) — keep reading the old dir
	}
	fs::path legacy = legacy_agent_dir();
	if ( fs::is_directory( legacy, ec ) ) {
		fs::create_directories( current.parent_path(), ec );
		if ( fs::rename( legacy, current, ec ), !ec ) return current;
		ec.clear();
		return legacy;
	}
	return current;
}

// --- Concrete project-local file paths ---

fs::path ProjectPaths::memory_dir() const
{
	return agent_dir() / "memory";
}

fs::path ProjectPaths::memory_file() const
{
	return memory_dir() / "memory.sqlite";
}

fs::path ProjectPaths::memory_file_json() const
{
	return memory_dir() / "memory.json";
}

fs::path ProjectPaths::sessions_dir() const
{
	return agent_dir() / "sessions";
}

fs::path ProjectPaths::session_dir( std::string_view session_id ) const
{
	return sessions_dir() / std::string( session_id );
}

fs::path ProjectPaths::scratch_file( std::string_view session_id ) const
{
	return session_dir( session_id ) / "scratch.json";
}

fs::path ProjectPaths::skills_active_dir() const
{
	return agent_dir() / "skills" / "active";
}

fs::path ProjectPaths::agents_subdir() const
{
	return agent_dir() / "agents";
}

fs::path ProjectPaths::lock_file() const
{
	return agent_dir() / ".lock";
}

std::vector<fs::path> ProjectPaths::skills_search_path( const Paths& paths ) const
{
	// Highest priority first — see project_paths.h docs.
	std::vector<fs::path> out;
	out.push_back( skills_active_dir() );
	out.push_back( paths.global_skills_dir() );
	for ( const auto& d : paths.system_data_dirs() ) {
		out.push_back( d / "skills" / "default" );
	}
	return out;
}

}  // namespace hive