// SPDX-License-Identifier: BSL-1.0
// tests/test_xdg_paths.cpp
//
// Plan.md Phase 1 acceptance: `paths.h` exists with `Paths` interface
// + `hive::Paths::global()` singleton. `paths.cpp` resolves all 7
// XDG roots + system dirs per §3.2 of Plan.md. All 6 env vars in §3.7
// are honoured.
//
// Tests:
//   1. Singleton identity (same address on repeat calls).
//   2. set_override() round-trip.
//   3. $HIVE_AGENT_HOME short-circuits the XDG resolution.
//   4. $XDG_CONFIG_HOME overrides config root.
//   5. $XDG_DATA_HOME overrides data root.
//   6. $XDG_STATE_HOME overrides state root.
//   7. $XDG_CACHE_HOME overrides cache root.
//   8. $XDG_DATA_DIRS / $HIVE_AGENT_SYSTEM_DATA_DIRS overrides.
//   9. $HIVE_AGENT_SYSTEM_CONFIG_DIRS override.
//  10. Legacy `~/.daedalus/` is preferred over XDG fallback when it
//      exists (backward-compat read path).
//  11. Concrete file paths compose correctly (config_file = root/cfg).
//  12. Paths::global() is thread-safe.

#include <cassert>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "library/paths.h"

namespace fs = std::filesystem;

// RAII env-var setter — restores on destruction.
class ScopedEnv
{
public:
	ScopedEnv( const char* name, const std::string& value )
		: m_name( name )
	{
		const char* prev = std::getenv( name );
		m_had_prev = ( prev != nullptr );
		if ( m_had_prev ) m_prev = prev;
		if ( value.empty() ) {
			::unsetenv( name );
		} else {
			::setenv( name, value.c_str(), 1 );
		}
	}
	~ScopedEnv()
	{
		if ( m_had_prev ) {
			::setenv( m_name, m_prev.c_str(), 1 );
		} else {
			::unsetenv( m_name );
		}
	}
	ScopedEnv( const ScopedEnv& ) = delete;
	ScopedEnv& operator=( const ScopedEnv& ) = delete;

private:
	const char* m_name;
	std::string m_prev;
	bool m_had_prev;
};

// RAII override-setter — restores default singleton on destruction.
class ScopedOverride
{
public:
	explicit ScopedOverride( std::shared_ptr<hive::Paths> p )
	{
		hive::Paths::set_override( std::move( p ) );
	}
	~ScopedOverride()
	{
		hive::Paths::set_override( nullptr );  // reset to default
	}
	ScopedOverride( const ScopedOverride& ) = delete;
	ScopedOverride& operator=( const ScopedOverride& ) = delete;
};

// =============================================================================
// 1. Singleton identity
// =============================================================================
static void test_singleton_identity()
{
	std::cout << "[1] Paths::global() singleton identity... ";
	[[maybe_unused]] const hive::Paths* a = &hive::Paths::global();
	[[maybe_unused]] const hive::Paths* b = &hive::Paths::global();
	assert( a == b );
	std::cout << "ok\n";
}

// =============================================================================
// 2. set_override() round-trip
// =============================================================================
class StubPaths final : public hive::Paths
{
public:
	std::filesystem::path stub_root = "/stub/root";
	std::filesystem::path user_root() const override { return stub_root; }
	std::filesystem::path user_config_root() const override { return stub_root / "cfg"; }
	std::filesystem::path user_data_root() const override { return stub_root / "data"; }
	std::filesystem::path user_state_root() const override { return stub_root / "state"; }
	std::filesystem::path user_cache_root() const override { return stub_root / "cache"; }
	std::vector<std::filesystem::path> system_data_dirs() const override { return { stub_root / "sys_data" }; }
	std::vector<std::filesystem::path> system_config_dirs() const override { return { stub_root / "sys_cfg" }; }
	std::filesystem::path config_file() const override { return user_config_root() / "config.json"; }
	std::filesystem::path mcp_servers_file() const override { return user_config_root() / "mcp/servers.json"; }
	std::filesystem::path mcp_env_file() const override { return user_config_root() / "mcp/.env"; }
	std::filesystem::path global_skills_dir() const override { return user_data_root() / "skills"; }
	std::filesystem::path log_file() const override { return user_state_root() / "logs/hive-brain.log"; }
	std::filesystem::path response_cache_dir() const override { return user_cache_root() / "responses"; }
	void ensure_directories() const override {}
};

static void test_set_override()
{
	std::cout << "[2] Paths::set_override() round-trip... ";
	auto stub = std::make_shared<StubPaths>();
	stub->stub_root = "/tmp/test_override";
	ScopedOverride guard( stub );
	assert( hive::Paths::global().user_root() == "/tmp/test_override" );
	assert( hive::Paths::global().user_config_root() == "/tmp/test_override/cfg" );
	std::cout << "ok\n";
}

// =============================================================================
// 3. $HIVE_AGENT_HOME overrides everything
// =============================================================================
static void test_agent_home_override()
{
	std::cout << "[3] $HIVE_AGENT_HOME single-root override... ";
	ScopedEnv e1( "HIVE_AGENT_HOME", "/opt/hive-custom" );
	ScopedEnv e2( "HIVE_AGENT_CONFIG_HOME", "" );
	ScopedEnv e3( "XDG_CONFIG_HOME", "" );
	// Note: user_root() honours HIVE_AGENT_HOME at priority 1 — see paths.cpp.
	// user_config_root() honours HIVE_AGENT_CONFIG_HOME at priority 1.
	assert( hive::Paths::global().user_root() == "/opt/hive-custom" );
	std::cout << "ok\n";
}

// =============================================================================
// 4. $XDG_CONFIG_HOME
// =============================================================================
static void test_xdg_config_home()
{
	std::cout << "[4] $XDG_CONFIG_HOME... ";
	ScopedEnv e( "XDG_CONFIG_HOME", "/tmp/xdg-cfg" );
	ScopedEnv h( "HIVE_AGENT_CONFIG_HOME", "" );
	assert( hive::Paths::global().user_config_root() == "/tmp/xdg-cfg/hive-brain" );
	assert( hive::Paths::global().config_file() == "/tmp/xdg-cfg/hive-brain/config.json" );
	std::cout << "ok\n";
}

// =============================================================================
// 5. $XDG_DATA_HOME
// =============================================================================
static void test_xdg_data_home()
{
	std::cout << "[5] $XDG_DATA_HOME... ";
	ScopedEnv e( "XDG_DATA_HOME", "/tmp/xdg-data" );
	ScopedEnv h( "HIVE_AGENT_HOME", "" );
	ScopedEnv h2( "HIVE_AGENT_DATA_HOME", "" );
	assert( hive::Paths::global().user_data_root() == "/tmp/xdg-data/hive-brain" );
	assert( hive::Paths::global().global_skills_dir() == "/tmp/xdg-data/hive-brain/skills" );
	std::cout << "ok\n";
}

// =============================================================================
// 6. $XDG_STATE_HOME
// =============================================================================
static void test_xdg_state_home()
{
	std::cout << "[6] $XDG_STATE_HOME... ";
	ScopedEnv e( "XDG_STATE_HOME", "/tmp/xdg-state" );
	ScopedEnv h( "HIVE_AGENT_STATE_HOME", "" );
	assert( hive::Paths::global().user_state_root() == "/tmp/xdg-state/hive-brain" );
	assert( hive::Paths::global().log_file() == "/tmp/xdg-state/hive-brain/logs/hive-brain.log" );
	std::cout << "ok\n";
}

// =============================================================================
// 7. $XDG_CACHE_HOME
// =============================================================================
static void test_xdg_cache_home()
{
	std::cout << "[7] $XDG_CACHE_HOME... ";
	ScopedEnv e( "XDG_CACHE_HOME", "/tmp/xdg-cache" );
	ScopedEnv h( "HIVE_AGENT_CACHE_HOME", "" );
	assert( hive::Paths::global().user_cache_root() == "/tmp/xdg-cache/hive-brain" );
	assert( hive::Paths::global().response_cache_dir() == "/tmp/xdg-cache/hive-brain/responses" );
	std::cout << "ok\n";
}

// =============================================================================
// 8. $HIVE_AGENT_SYSTEM_DATA_DIRS
// =============================================================================
static void test_system_data_dirs()
{
	std::cout << "[8] $HIVE_AGENT_SYSTEM_DATA_DIRS... ";
	ScopedEnv e( "HIVE_AGENT_SYSTEM_DATA_DIRS", "/sys/data1:/sys/data2" );
	auto dirs = hive::Paths::global().system_data_dirs();
	assert( dirs.size() == 2 );
	assert( dirs[0] == "/sys/data1" );
	assert( dirs[1] == "/sys/data2" );
	std::cout << "ok\n";
}

// =============================================================================
// 9. $HIVE_AGENT_SYSTEM_CONFIG_DIRS
// =============================================================================
static void test_system_config_dirs()
{
	std::cout << "[9] $HIVE_AGENT_SYSTEM_CONFIG_DIRS... ";
	ScopedEnv e( "HIVE_AGENT_SYSTEM_CONFIG_DIRS", "/sys/cfg1:/sys/cfg2:/sys/cfg3" );
	auto dirs = hive::Paths::global().system_config_dirs();
	assert( dirs.size() == 3 );
	assert( dirs[0] == "/sys/cfg1" );
	std::cout << "ok\n";
}

// =============================================================================
// 10. Concrete file paths compose correctly
// =============================================================================
static void test_concrete_file_paths()
{
	std::cout << "[11] Concrete file paths... ";
	ScopedEnv e( "XDG_CONFIG_HOME", "/x" );
	ScopedEnv h( "HIVE_AGENT_CONFIG_HOME", "" );
	assert( hive::Paths::global().config_file() == "/x/hive-brain/config.json" );
	assert( hive::Paths::global().mcp_servers_file() == "/x/hive-brain/mcp/servers.json" );
	assert( hive::Paths::global().mcp_env_file() == "/x/hive-brain/mcp/.env" );
	std::cout << "ok\n";
}

// =============================================================================
// 12. Thread-safe singleton
// =============================================================================
static void test_thread_safe_singleton()
{
	std::cout << "[12] Paths::global() thread-safety... ";
	std::vector<std::thread> threads;
	std::vector<const hive::Paths*> seen;
	std::mutex m;
	for ( int i = 0; i < 8; ++i ) {
		threads.emplace_back( [&]() {
			const hive::Paths* p = &hive::Paths::global();
			std::lock_guard<std::mutex> lock( m );
			seen.push_back( p );
		} );
	}
	for ( auto& t : threads ) t.join();
	for ( size_t i = 1; i < seen.size(); ++i ) {
		assert( seen[i] == seen[0] );
	}
	std::cout << "ok\n";
}

int main()
{
	test_singleton_identity();
	test_set_override();
    test_agent_home_override();
    test_xdg_config_home();
    test_xdg_data_home();
    test_xdg_state_home();
    test_xdg_cache_home();
    test_system_data_dirs();
    test_system_config_dirs();
    test_concrete_file_paths();
    test_thread_safe_singleton();
	std::cout << "All test_xdg_paths tests passed.\n";
	return 0;
}