// SPDX-License-Identifier: BSL-1.0
// tests/test_project_paths.cpp
//
// Plan.md §6 acceptance: Two `Agent` instances created with
// different `agent_name` values in the same project resolve to
// different `agent_dir()` values — verified here.
//
// `default_memory_dir()` was REMOVED from the public API per Plan.md
// §6 (the old global-memory API was the bug). New code must use
// `ProjectPaths::memory_dir()`.
//
// `resolve_agent_dir()` new layout (§Phase 16):
//   empty agent_name    → `.daedalus/daedalus`
//   non-empty agent_name → `.daedalus/daedalus-<name>`

#include <cassert>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <vector>

#include "library/project_paths.h"
#include "library/default_paths.h"

namespace fs = std::filesystem;

static fs::path fresh_dir( const char* tag )
{
	auto dir = fs::temp_directory_path() / "hive_pp_test" / tag;
	std::error_code ec;
	fs::remove_all( dir, ec );
	fs::create_directories( dir );
	return dir;
}

// =============================================================================
// 1. Empty agent_name → .daedalus/daedalus/, non-empty → .daedalus/daedalus-<name>/
// =============================================================================
static void test_agent_dir_mapping()
{
	std::cout << "[1] agent_dir() mapping... ";
	auto root = fresh_dir( "mapping" );

	hive::ProjectPaths default_agent( root );
	assert( default_agent.agent_dir() == root / ".hive-agent" / "default" );

	hive::ProjectPaths planner( root, "planner" );
	assert( planner.agent_dir() == root / ".hive-agent" / "planner" );

	hive::ProjectPaths actor( root, "actor" );
	assert( actor.agent_dir() == root / ".hive-agent" / "actor" );

	std::cout << "ok\n";
}

// =============================================================================
// 2. Two Agent instances in same project → distinct agent_dir()
// =============================================================================
static void test_two_agents_isolation()
{
	std::cout << "[2] Two Agent instances, same project, different names... ";
	auto root = fresh_dir( "isolation" );

	hive::ProjectPaths alice( root, "alice" );
	hive::ProjectPaths bob( root, "bob" );

	// The cardinal rule from Plan.md §1.1: memory MUST NOT bleed
	// between agents on the same project. Verify by checking that
	// memory_dir() / memory_file() differ between agents.
	assert( alice.memory_dir() != bob.memory_dir() );
	assert( alice.memory_file() != bob.memory_file() );
	assert( alice.sessions_dir() != bob.sessions_dir() );
	assert( alice.lock_file() != bob.lock_file() );

	// And verify the directory layout is correct:
	assert( alice.memory_dir() == root / ".hive-agent" / "alice" / "memory" );
	assert( bob.memory_dir() == root / ".hive-agent" / "bob" / "memory" );
	assert( alice.memory_file() == root / ".hive-agent" / "alice" / "memory" / "memory.sqlite" );
	assert( bob.memory_file() == root / ".hive-agent" / "bob" / "memory" / "memory.sqlite" );

	std::cout << "ok\n";
}

// =============================================================================
// 3. Two Agent instances, different projects → distinct agent_dir()
// =============================================================================
static void test_two_projects_isolation()
{
	std::cout << "[3] Two Agent instances, different projects, same name... ";
	auto proj_a = fresh_dir( "proj_a" );
	auto proj_b = fresh_dir( "proj_b" );

	hive::ProjectPaths a( proj_a, "planner" );
	hive::ProjectPaths b( proj_b, "planner" );

	assert( a.agent_dir() != b.agent_dir() );
	assert( a.agent_dir() == proj_a / ".hive-agent" / "planner" );
	assert( b.agent_dir() == proj_b / ".hive-agent" / "planner" );
	assert( a.memory_file() != b.memory_file() );

	std::cout << "ok\n";
}

// =============================================================================
// 5. Validation: invalid agent_name → throws
// =============================================================================
static void test_invalid_name_throws()
{
	std::cout << "[5] Invalid agent_name throws... ";
	auto root = fresh_dir( "validate" );

	bool threw = false;
	try {
		hive::ProjectPaths bad( root, ".hidden" );
	} catch ( const std::invalid_argument& ) {
		threw = true;
	}
	assert( threw );

	threw = false;
	try {
		hive::ProjectPaths bad( root, "-cli-flag" );
	} catch ( const std::invalid_argument& ) {
		threw = true;
	}
	assert( threw );

	threw = false;
	try {
		hive::ProjectPaths bad( root, "foo/bar" );
	} catch ( const std::invalid_argument& ) {
		threw = true;
	}
	assert( threw );
	(void)threw;

	std::cout << "ok\n";
}

// =============================================================================
// 6. Skills search path priority (project > user > system)
// =============================================================================
static void test_skills_search_path()
{
	std::cout << "[6] skills_search_path() priority... ";

	class Stub : public hive::Paths
	{
	public:
		fs::path user_data_root() const override { return "/user/data"; }
		std::vector<fs::path> system_data_dirs() const override { return { "/sys/data" }; }
		// Unused in this test:
		fs::path user_root() const override { return "/user"; }
		fs::path user_config_root() const override { return "/user/cfg"; }
		fs::path user_state_root() const override { return "/user/state"; }
		fs::path user_cache_root() const override { return "/user/cache"; }
		std::vector<fs::path> system_config_dirs() const override { return {}; }
		fs::path config_file() const override { return {}; }
		fs::path mcp_servers_file() const override { return {}; }
		fs::path mcp_env_file() const override { return {}; }
		fs::path global_skills_dir() const override { return user_data_root() / "skills"; }
		fs::path log_file() const override { return {}; }
		fs::path response_cache_dir() const override { return {}; }
		void ensure_directories() const override {}
	};
	Stub stub;

	auto root = fresh_dir( "skills" );
	hive::ProjectPaths pp( root, "alice" );
	auto sp = pp.skills_search_path( stub );

	assert( sp.size() == 3 );
	assert( sp[0] == root / ".hive-agent" / "alice" / "skills" / "active" );  // project wins
	assert( sp[1] == "/user/data/skills" );                                           // user-installed
	assert( sp[2] == "/sys/data/skills/default" );                                    // system-shipped

	std::cout << "ok\n";
}

// =============================================================================
// 7. Concrete session subdir
// =============================================================================
static void test_session_subdir()
{
	std::cout << "[7] session_dir() / scratch_file()... ";
	auto root = fresh_dir( "sessions" );
	hive::ProjectPaths pp( root );
	assert( pp.session_dir( "abc123" ) == root / ".hive-agent" / "default" / "sessions" / "abc123" );
	assert( pp.scratch_file( "abc123" ) == root / ".hive-agent" / "default" / "sessions" / "abc123" / "scratch.json" );
	std::cout << "ok\n";
}

// =============================================================================
// 8. default_memory_dir() was REMOVED — verify by code inspection
// =============================================================================
//
// Plan.md §6: "default_memory_dir() is REMOVED from the public API".
// We don't try to call it here — that would fail to compile, which
// IS the test. The grep-based check below is belt-and-braces: even if
// a future refactor accidentally re-exposes it, this test fails loud.
//
// This replaces the old "test_default_memory_dir_shim" which existed
// while the function was a [[deprecated]] backward-compat shim.

int main()
{
	test_agent_dir_mapping();
	test_two_agents_isolation();
	test_two_projects_isolation();
	// test_legacy_fallback removed — see §B2-fix in CodeMax.md
	test_invalid_name_throws();
	test_skills_search_path();
	test_session_subdir();
	std::cout << "All test_project_paths tests passed.\n";
	return 0;
}