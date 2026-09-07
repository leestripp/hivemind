/**
 * @file paths.cpp
 * @brief Default `Paths` implementation — XDG-compliant (Plan.md §3.2).
 *
 * Resolution algorithm (per Plan.md §3.2 + §3.7):
 *
 *   user_root() priority:
 *     1. $HIVE_AGENT_HOME          (single-root override, like $GOPATH)
 *     2. $XDG_DATA_HOME/hive-brain (XDG-compliant)
 *     3. $HOME/.daedalus         (LEGACY, deprecated)
 *     4. $HOME/.local/share/hive-brain (XDG fallback when neither set)
 *
 *   user_config_root() priority:
 *     1. $HIVE_AGENT_CONFIG_HOME
 *     2. $XDG_CONFIG_HOME/hive-brain
 *     3. $HOME/.config/hive-brain
 *
 *   user_data_root() priority:
 *     1. $HIVE_AGENT_DATA_HOME
 *     2. $XDG_DATA_HOME/hive-brain
 *     3. $HOME/.local/share/hive-brain
 *
 *   user_state_root() priority:
 *     1. $HIVE_AGENT_STATE_HOME
 *     2. $XDG_STATE_HOME/hive-brain
 *     3. $HOME/.local/state/hive-brain
 *
 *   user_cache_root() priority:
 *     1. $HIVE_AGENT_CACHE_HOME
 *     2. $XDG_CACHE_HOME/hive-brain
 *     3. $HOME/.cache/hive-brain
 *
 *   system_data_dirs()   priority:
 *     1. $HIVE_AGENT_SYSTEM_DATA_DIRS   (colon-separated)
 *     2. /usr/local/share/hive-brain:/usr/share/hive-brain
 *
 *   system_config_dirs() priority:
 *     1. $HIVE_AGENT_SYSTEM_CONFIG_DIRS
 *     2. /etc/xdg/hive-brain
 *
 * §Phase 12: legacy `~/.daedalus/` is honoured as a read-fallback
 * in `user_root()` so existing installs keep working for one release
 * cycle. The deprecation warning is printed by the CLI host, not by
 * this class — keeps the library free of stderr writes (see Agent.md
 * §Phase 5.1, no-stdio-in-library rule).
 */
#include "library/paths.h"

#include <cstdlib>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>

namespace hive {

namespace {

namespace fs = std::filesystem;

/// Safe getenv — empty string when unset.
std::string env_or_empty( const char* name )
{
	const char* v = std::getenv( name );
	return ( v && *v ) ? std::string( v ) : std::string{};
}

/// Path-or-empty: returns the env value if set and non-empty,
/// otherwise `std::nullopt` semantics via empty path.
fs::path env_path_or_empty( const char* name )
{
	std::string v = env_or_empty( name );
	return v.empty() ? fs::path{} : fs::path{ v };
}

fs::path home_or_tmp()
{
	std::string home = env_or_empty( "HOME" );
	return home.empty() ? fs::path{ "/tmp" } : fs::path{ home };
}

/// Split "a:b:c" → ["a","b","c"]. Empty entries dropped.
std::vector<fs::path> split_path_list( const std::string& s )
{
	std::vector<fs::path> out;
	std::stringstream ss( s );
	std::string item;
	while ( std::getline( ss, item, ':' ) ) {
		if ( !item.empty() ) {
			out.emplace_back( item );
		}
	}
	return out;
}

class DefaultPaths final : public Paths
{
public:
	fs::path user_root() const override
	{
		// 1. $HIVE_AGENT_HOME — single-root override.
		if ( fs::path p = env_path_or_empty( "HIVE_AGENT_HOME" ); !p.empty() ) {
			return p;
		}
		// 2. $XDG_DATA_HOME/hive-brain — XDG-compliant default.
		if ( fs::path xdg = env_path_or_empty( "XDG_DATA_HOME" ); !xdg.empty() ) {
			return xdg / "hive-brain";
		}
		// 3. $HOME/.daedalus — LEGACY (deprecated).
		// 4. $HOME/.local/share/hive-brain — XDG fallback.
		//
		// We can't distinguish #3 from #4 at resolution time without
		// filesystem probes (Plan.md §3.4 says we MUST prefer legacy
		// when it exists so existing installs keep working). We probe
		// here and document the rule; the probing is a single stat()
		// call.
		fs::path legacy = home_or_tmp() / ".daedalus";
		std::error_code ec;
		if ( fs::is_directory( legacy, ec ) ) {
			return legacy;
		}
		return home_or_tmp() / ".local" / "share" / "hive-brain";
	}

	fs::path user_config_root() const override
	{
		if ( fs::path p = env_path_or_empty( "HIVE_AGENT_CONFIG_HOME" ); !p.empty() ) {
			return p;
		}
		if ( fs::path xdg = env_path_or_empty( "XDG_CONFIG_HOME" ); !xdg.empty() ) {
			return xdg / "hive-brain";
		}
		return home_or_tmp() / ".config" / "hive-brain";
	}

	fs::path user_data_root() const override
	{
		if ( fs::path p = env_path_or_empty( "HIVE_AGENT_DATA_HOME" ); !p.empty() ) {
			return p;
		}
		if ( fs::path xdg = env_path_or_empty( "XDG_DATA_HOME" ); !xdg.empty() ) {
			return xdg / "hive-brain";
		}
		return home_or_tmp() / ".local" / "share" / "hive-brain";
	}

	fs::path user_state_root() const override
	{
		if ( fs::path p = env_path_or_empty( "HIVE_AGENT_STATE_HOME" ); !p.empty() ) {
			return p;
		}
		if ( fs::path xdg = env_path_or_empty( "XDG_STATE_HOME" ); !xdg.empty() ) {
			return xdg / "hive-brain";
		}
		return home_or_tmp() / ".local" / "state" / "hive-brain";
	}

	fs::path user_cache_root() const override
	{
		if ( fs::path p = env_path_or_empty( "HIVE_AGENT_CACHE_HOME" ); !p.empty() ) {
			return p;
		}
		if ( fs::path xdg = env_path_or_empty( "XDG_CACHE_HOME" ); !xdg.empty() ) {
			return xdg / "hive-brain";
		}
		return home_or_tmp() / ".cache" / "hive-brain";
	}

	std::vector<fs::path> system_data_dirs() const override
	{
		std::string override = env_or_empty( "HIVE_AGENT_SYSTEM_DATA_DIRS" );
		if ( !override.empty() ) {
			return split_path_list( override );
		}
		return { fs::path{ "/usr/local/share/hive-brain" },
		         fs::path{ "/usr/share/hive-brain" } };
	}

	std::vector<fs::path> system_config_dirs() const override
	{
		std::string override = env_or_empty( "HIVE_AGENT_SYSTEM_CONFIG_DIRS" );
		if ( !override.empty() ) {
			return split_path_list( override );
		}
		return { fs::path{ "/etc/xdg/hive-brain" } };
	}

	// --- Concrete file paths ---

	fs::path config_file() const override
	{
		return user_config_root() / "config.json";
	}

	fs::path mcp_servers_file() const override
	{
		return user_config_root() / "mcp" / "servers.json";
	}

	fs::path mcp_env_file() const override
	{
		return user_config_root() / "mcp" / ".env";
	}

	fs::path global_skills_dir() const override
	{
		return user_data_root() / "skills";
	}

	fs::path log_file() const override
	{
		return user_state_root() / "logs" / "hive-brain.log";
	}

	fs::path response_cache_dir() const override
	{
		return user_cache_root() / "responses";
	}

	// OmniNet paths: prefer XDG, but honour the legacy ~/.omninet
	// directory if it already exists so existing OmniNet installs keep
	// working for one release cycle.
	fs::path omninet_config_file() const override
	{
		fs::path legacy = home_or_tmp() / ".omninet" / "config.json";
		std::error_code ec;
		if ( fs::is_regular_file( legacy, ec ) ) {
			return legacy;
		}
		return user_config_root() / "omninet" / "config.json";
	}

	fs::path omninet_data_dir() const override
	{
		fs::path legacy = home_or_tmp() / ".omninet";
		std::error_code ec;
		if ( fs::is_directory( legacy, ec ) ) {
			return legacy;
		}
		return user_data_root() / "omninet";
	}

	// Plan.md §5.1 risk mitigation: enforce directory permissions.
	//   config_root  → 0750  (LLM keys are sensitive; group-readable is
	//                       the minimum that lets a daemon group share)
	//   data_root    → 0750  (skill packs may carry license-sensitive
	//                       metadata; same reasoning)
	//   state_root   → 0700  (logs may include API-key fragments,
	//                       history may contain user messages)
	//   cache_root   → 0700  (response replay cache may contain PII)
	void ensure_directories() const override
	{
		auto make = []( const fs::path& root,
		                fs::perms perms ) {
			std::error_code ec;
			if ( !fs::exists( root, ec ) ) {
				fs::create_directories( root, ec );
				if ( ec ) return;  // silent — caller will surface
				                    // I/O errors on first write.
			}
			// Apply permissions unconditionally; if the dir was
			// pre-existing with looser perms, tighten. Idempotent.
			fs::permissions( root,
			                 perms,
			                 fs::perm_options::replace,
			                 ec );
			// ec silently ignored — permissions() can fail on filesystems
			// that don't support chmod (e.g. FAT32 mounts), and the
			// library treats that as best-effort.
		};
		make( user_config_root(), fs::perms::owner_all
		                       | fs::perms::group_read
		                       | fs::perms::group_exec );  // 0750
		make( user_data_root(),   fs::perms::owner_all
		                       | fs::perms::group_read
		                       | fs::perms::group_exec );  // 0750
		make( user_state_root(),  fs::perms::owner_all );   // 0700
		make( user_cache_root(),  fs::perms::owner_all );   // 0700
	}
};

}  // namespace

std::unique_ptr<Paths> Paths::make_default()
{
	return std::make_unique<DefaultPaths>();
}

std::unique_ptr<Paths> make_default_paths()
{
	return Paths::make_default();
}

namespace {
std::shared_ptr<Paths>& mutable_singleton() noexcept
{
	static std::shared_ptr<Paths> g_default = Paths::make_default();
	return g_default;
}

std::mutex& singleton_mutex() noexcept
{
	static std::mutex m;
	return m;
}
}  // namespace

const Paths& Paths::global() noexcept
{
	std::lock_guard<std::mutex> lock( singleton_mutex() );
	return *mutable_singleton();
}

void Paths::set_override( std::shared_ptr<Paths> p ) noexcept
{
	std::lock_guard<std::mutex> lock( singleton_mutex() );
	mutable_singleton() = p ? std::move( p ) : Paths::make_default();
}

}  // namespace hive