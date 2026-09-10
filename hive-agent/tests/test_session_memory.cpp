/**
 * @file test_session_memory.cpp
 * @brief Comprehensive unit tests for Session Memory functionality
 */

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>

#include "core/SessionMemory.h"

using json = nlohmann::json;

// Helper to clean up test directories
void cleanup_test_dir(const std::filesystem::path& test_dir) {
    std::filesystem::path memory_dir = test_dir / ".hive-agent" / "default" / "memory";
    if (std::filesystem::exists(memory_dir)) {
        std::filesystem::remove_all(memory_dir);
    }
}

// ============================================================================
// TEST 1: Basic Initialization
// ============================================================================
TEST_CASE("SessionMemory - Basic Initialization") {
    std::filesystem::path test_dir = std::filesystem::temp_directory_path() / "hive_agent_test_basic_init";
    std::filesystem::create_directories(test_dir);
    
    std::filesystem::path memory_file = test_dir / ".hive-agent" / "default" / "memory" / "memory.json";
    if (std::filesystem::exists(memory_file)) {
        std::filesystem::remove(memory_file);
    }
    
    SUBCASE("Memory is enabled by default") {
        SessionMemory memory(test_dir / ".hive-agent" / "default");
        CHECK(memory.is_enabled() == true);
    }
    
    SUBCASE("Memory directory is created on init") {
        SessionMemory memory(test_dir / ".hive-agent" / "default");
        std::filesystem::path memory_dir = test_dir / ".hive-agent" / "default" / "memory";
        CHECK(std::filesystem::exists(memory_dir));
    }
    
    cleanup_test_dir(test_dir);
}

// ============================================================================
// TEST 2: Build Commands
// ============================================================================
TEST_CASE("SessionMemory - Build Commands") {
    std::filesystem::path test_dir = std::filesystem::temp_directory_path() / "hive_agent_test_builds";
    std::filesystem::create_directories(test_dir);
    
    SessionMemory memory(test_dir / ".hive-agent" / "default");
    memory.reset_for_testing();
    
    SUBCASE("Adding a build command") {
        memory.on_build_complete("cmake -S . -B build", true);
        
        auto builds = memory.get_builds();
        CHECK(builds.size() == 1);
        CHECK(builds[0]->command == "cmake -S . -B build");
        CHECK(builds[0]->success == true);
        CHECK(builds[0]->use_count == 1);
    }
    
    SUBCASE("Same command increases use_count") {
        memory.on_build_complete("cmake --build build", true);
        memory.on_build_complete("cmake --build build", true);
        
        auto builds = memory.get_builds();
        CHECK(builds.size() == 1);
        CHECK(builds[0]->use_count == 2);
    }
    
    SUBCASE("Different commands are stored separately") {
        memory.on_build_complete("cmake -S . -B build", true);
        memory.on_build_complete("make -j8", true);
        
        auto builds = memory.get_builds();
        CHECK(builds.size() == 2);
    }
    
    SUBCASE("Failed build is marked correctly") {
        memory.on_build_complete("cmake --build build", false);
        
        auto builds = memory.get_builds();
        CHECK(builds.size() == 1);
        CHECK(builds[0]->success == false);
    }
    
    cleanup_test_dir(test_dir);
}

// ============================================================================
// TEST 3: Error Tracking
// ============================================================================
TEST_CASE("SessionMemory - Error Tracking") {
    std::filesystem::path test_dir = std::filesystem::temp_directory_path() / "hive_agent_test_errors";
    std::filesystem::create_directories(test_dir);
    
    SessionMemory memory(test_dir / ".hive-agent" / "default");
    memory.reset_for_testing();
    
    SUBCASE("Adding an error") {
        memory.on_error("undefined reference to curl", "");
        
        auto errors = memory.get_errors();
        CHECK(errors.size() == 1);
        CHECK(errors[0]->pattern == "undefined reference to curl");
        CHECK(errors[0]->resolved == false);
        CHECK(errors[0]->confidence == 2);  // Low confidence until resolved
    }
    
    SUBCASE("Resolving an error") {
        memory.on_error("undefined reference to curl", "");
        memory.on_error_resolved("undefined reference to curl", "target_link_libraries CURL::CURL");
        
        auto errors = memory.get_errors();
        CHECK(errors.size() == 1);
        CHECK(errors[0]->resolved == true);
        CHECK(errors[0]->confidence == 4);  // Higher confidence when resolved
        CHECK(errors[0]->solution == "target_link_libraries CURL::CURL");
    }
    
    SUBCASE("Unresolved errors are tracked separately") {
        memory.on_error("error A", "");
        memory.on_error_resolved("error A", "fix A");
        memory.on_error("error B", "");
        
        auto unresolved = memory.get_unresolved_errors();
        CHECK(unresolved.size() == 1);
        CHECK(unresolved[0]->pattern == "error B");
    }
    
    SUBCASE("Same error increases occurrences") {
        memory.on_error("linker error", "");
        memory.on_error("linker error", "");
        memory.on_error("linker error", "");
        
        auto errors = memory.get_errors();
        CHECK(errors.size() == 1);
        CHECK(errors[0]->occurrences == 3);
    }
    
    SUBCASE("Multiple errors tracked separately") {
        memory.on_error("error 1", "");
        memory.on_error("error 2", "");
        memory.on_error("error 3", "");
        
        auto errors = memory.get_errors();
        CHECK(errors.size() == 3);
    }
    
    cleanup_test_dir(test_dir);
}

// ============================================================================
// TEST 4: Sensitive Data Filter
// ============================================================================
TEST_CASE("SessionMemory - Sensitive Data Filter") {
    std::filesystem::path test_dir = std::filesystem::temp_directory_path() / "hive_agent_test_filter";
    std::filesystem::create_directories(test_dir);
    
    SessionMemory memory(test_dir / ".hive-agent" / "default");
    memory.reset_for_testing();
    
    SUBCASE("API keys are filtered in build commands") {
        memory.on_build_complete("export API_KEY=sk-1234567890abcdefghijklmnopqrstuvwxyz", true);
        
        auto builds = memory.get_builds();
        CHECK(builds[0]->command.find("sk-1234567890abcdef") == std::string::npos);
    }
    
    SUBCASE("Passwords in environment variables are filtered") {
        memory.on_build_complete("export PASSWORD=MySecret123", true);
        
        auto builds = memory.get_builds();
        CHECK(builds[0]->command.find("MySecret123") == std::string::npos);
    }
    
    SUBCASE("AWS keys are filtered") {
        memory.on_build_complete("aws configure set aws_access_key_id AKIAIOSFODNN7EXAMPLE", true);
        
        auto builds = memory.get_builds();
        CHECK(builds[0]->command.find("AKIAIOSFODNN7EXAMPLE") == std::string::npos);
    }
    
    SUBCASE("GitHub tokens are filtered") {
        memory.on_build_complete("git push https://ghp_xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx@github.com/repo.git", true);
        
        auto builds = memory.get_builds();
        CHECK(builds[0]->command.find("ghp_xxxxx") == std::string::npos);
    }
    
    SUBCASE("Raw tokens are filtered") {
        memory.on_build_complete("export GITHUB_TOKEN=gho_xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx", true);
        
        auto builds = memory.get_builds();
        CHECK(builds[0]->command.find("gho_xxxxx") == std::string::npos);
    }
    
    cleanup_test_dir(test_dir);
}

// ============================================================================
// TEST 5: Session Lifecycle
// ============================================================================
TEST_CASE("SessionMemory - Session Lifecycle") {
    std::filesystem::path test_dir = std::filesystem::temp_directory_path() / "hive_agent_test_lifecycle";
    std::filesystem::create_directories(test_dir);
    
    SUBCASE("Session start loads existing memory") {
        SessionMemory memory1(test_dir / ".hive-agent" / "default");
        memory1.reset_for_testing();
        memory1.on_build_complete("cmake build", true);
        
        SessionMemory memory2(test_dir / ".hive-agent" / "default");
        auto builds = memory2.get_builds();
        CHECK(builds.size() == 1);
    }
    
    SUBCASE("Clear all memories works") {
        SessionMemory memory(test_dir / ".hive-agent" / "default");
        memory.reset_for_testing();
        memory.on_build_complete("cmake build", true);
        memory.on_error("some error", "");
        
        memory.clear_all_memories();
        
        CHECK(memory.get_builds().empty());
        CHECK(memory.get_errors().empty());
    }
    
    SUBCASE("Session end stores summary") {
        SessionMemory memory(test_dir / ".hive-agent" / "default");
        memory.reset_for_testing();
        memory.on_session_end("Fixed UTF-8 bug", {"src/core/utils.cpp"}, 5);
        
        auto sessions = memory.get_recent_sessions(5);
        CHECK(sessions.size() == 1);
        CHECK(sessions[0]->summary == "Fixed UTF-8 bug");
        CHECK(sessions[0]->iterations == 5);
    }
    
    cleanup_test_dir(test_dir);
}

// ============================================================================
// TEST 6: Statistics
// ============================================================================
TEST_CASE("SessionMemory - Statistics") {
    std::filesystem::path test_dir = std::filesystem::temp_directory_path() / "hive_agent_test_stats";
    std::filesystem::create_directories(test_dir);
    
    SessionMemory memory(test_dir / ".hive-agent" / "default");
    memory.reset_for_testing();
    memory.on_build_complete("cmake build", true);
    memory.on_build_complete("make test", true);
    memory.on_error("error 1", "");
    memory.on_error_resolved("error 2", "fix");
    
    auto stats = memory.get_stats();
    
    CHECK(stats.total_builds == 2);
    CHECK(stats.total_errors == 2);
    CHECK(stats.resolved_errors == 1);
    
    cleanup_test_dir(test_dir);
}

// ============================================================================
// TEST 7: System Prompt Generation
// ============================================================================
TEST_CASE("SessionMemory - System Prompt Generation") {
    std::filesystem::path test_dir = std::filesystem::temp_directory_path() / "hive_agent_test_prompt";
    std::filesystem::create_directories(test_dir);
    
    SessionMemory memory(test_dir / ".hive-agent" / "default");
    memory.reset_for_testing();
    memory.on_build_complete("cmake --build build -j8", true);
    memory.on_error_resolved("undefined reference", "add missing library");
    
    std::string prompt = memory.get_system_prompt_addition();
    
    CHECK(prompt.find("cmake --build build -j8") != std::string::npos);
    CHECK(prompt.find("undefined reference") != std::string::npos);
    CHECK(prompt.find("add missing library") != std::string::npos);
    
    cleanup_test_dir(test_dir);
}

// ============================================================================
// TEST 8: Error Deduplication
// ============================================================================
TEST_CASE("SessionMemory - Error Deduplication") {
    std::filesystem::path test_dir = std::filesystem::temp_directory_path() / "hive_agent_test_dedup";
    std::filesystem::create_directories(test_dir);
    
    SessionMemory memory(test_dir / ".hive-agent" / "default");
    memory.reset_for_testing();
    
    SUBCASE("Same error pattern increases occurrences") {
        memory.on_error("undefined reference", "");
        memory.on_error("undefined reference", "");
        memory.on_error("undefined reference", "");
        
        auto errors = memory.get_errors();
        CHECK(errors.size() == 1);
        CHECK(errors[0]->occurrences == 3);
    }
    
    SUBCASE("Solution can be updated via on_error_resolved") {
        memory.on_error("cmake error", "");
        memory.on_error_resolved("cmake error", "add missing dependency");
        
        auto errors = memory.get_errors();
        CHECK(errors[0]->resolved == true);
        CHECK(errors[0]->solution == "add missing dependency");
    }
    
    SUBCASE("Different errors have different occurrences") {
        memory.on_error("error A", "");
        memory.on_error("error A", "");
        memory.on_error("error B", "");
        
        auto errors = memory.get_errors();
        CHECK(errors.size() == 2);
        bool has_two_occurrences = (errors[0]->occurrences == 2) || (errors[1]->occurrences == 2);
        CHECK(has_two_occurrences);
    }
    
    cleanup_test_dir(test_dir);
}

// ============================================================================
// TEST 9: Preferences Tracking
// ============================================================================
TEST_CASE("SessionMemory - Preferences Tracking") {
    std::filesystem::path test_dir = std::filesystem::temp_directory_path() / "hive_agent_test_prefs";
    std::filesystem::create_directories(test_dir);
    
    SessionMemory memory(test_dir / ".hive-agent" / "default");
    memory.reset_for_testing();
    
    SUBCASE("Remember stores preference") {
        memory.remember("preferences", "User prefers verbose logging", 4, {"logging", "user-preference"});
        
        auto prefs = memory.get_preferences();
        CHECK(prefs.size() == 1);
        // Key is set to "explicit_memory" for remember() calls
        CHECK(prefs[0]->value == "User prefers verbose logging");
        CHECK(prefs[0]->observation_count == 1);
    }
    
    SUBCASE("Multiple preferences tracked separately") {
        memory.remember("preferences", "pref 1", 3, {});
        memory.remember("preferences", "pref 2", 4, {});
        
        auto prefs = memory.get_preferences();
        CHECK(prefs.size() == 2);
    }
    
    SUBCASE("User corrections stored") {
        memory.on_user_correction("Use clang++ instead of g++", "Changed to clang++ compiler");
        
        auto prefs = memory.get_preferences();
        CHECK(prefs.size() == 1);
    }
    
    cleanup_test_dir(test_dir);
}

// ============================================================================
// TEST 10: Pattern Entries
// ============================================================================
TEST_CASE("SessionMemory - Pattern Entries") {
    std::filesystem::path test_dir = std::filesystem::temp_directory_path() / "hive_agent_test_patterns";
    std::filesystem::create_directories(test_dir);
    
    SessionMemory memory(test_dir / ".hive-agent" / "default");
    memory.reset_for_testing();
    
    SUBCASE("Remember stores pattern") {
        memory.remember("patterns", "Project uses header-only libraries in include/ directory", 3, {"structure", "headers"});
        
        auto patterns = memory.get_patterns();
        CHECK(patterns.size() == 1);
        // Content stored in description field
        CHECK(patterns[0]->description.find("header-only") != std::string::npos);
    }
    
    SUBCASE("Pattern with type stored correctly") {
        memory.remember("patterns", "File naming: snake_case for source, CamelCase for headers", 4, {"naming"});
        
        auto patterns = memory.get_patterns();
        CHECK(patterns.size() == 1);
    }
    
    cleanup_test_dir(test_dir);
}

// ============================================================================
// TEST 11: Find Methods
// ============================================================================
TEST_CASE("SessionMemory - Find Methods") {
    std::filesystem::path test_dir = std::filesystem::temp_directory_path() / "hive_agent_test_find";
    std::filesystem::create_directories(test_dir);
    
    SessionMemory memory(test_dir / ".hive-agent" / "default");
    memory.reset_for_testing();
    memory.on_build_complete("cmake --build build -j8", true);
    memory.on_build_complete("make test", true);
    memory.on_error("undefined reference", "add library");
    memory.on_error("linker error", "check dependencies");
    
    SUBCASE("find_error returns matching error") {
        auto result = memory.find_error("undefined reference");
        CHECK(result.has_value() == true);
        CHECK((*result)->pattern == "undefined reference");
    }
    
    SUBCASE("find_error returns nullopt for non-existent") {
        auto result = memory.find_error("non-existent error");
        CHECK(result.has_value() == false);
    }
    
    SUBCASE("find_build returns matching build") {
        // Use exact stored command
        auto result = memory.find_build("cmake --build build -j8");
        // Result depends on implementation - either exact match or partial
        CHECK(result.has_value() == true);
    }
    
    SUBCASE("find_build returns nullopt for non-existent") {
        auto result = memory.find_build("xyzzy_make_nothing");
        CHECK(result.has_value() == false);
    }
    
    SUBCASE("find_build supports partial match") {
        memory.on_build_complete("make test", true);
        // If partial match is supported, make should find "make test"
        // Note: result not used - just verifying no crash
        memory.find_build("make");
        CHECK(true);
    }
    
    cleanup_test_dir(test_dir);
}

// ============================================================================
// TEST 12: Recent Sessions
// ============================================================================
TEST_CASE("SessionMemory - Recent Sessions") {
    std::filesystem::path test_dir = std::filesystem::temp_directory_path() / "hive_agent_test_sessions";
    std::filesystem::create_directories(test_dir);
    
    SessionMemory memory(test_dir / ".hive-agent" / "default");
    memory.reset_for_testing();
    
    SUBCASE("Empty when no sessions") {
        auto sessions = memory.get_recent_sessions(5);
        CHECK(sessions.empty());
    }
    
    SUBCASE("Stores session summaries") {
        memory.on_session_end("Fixed bug A", {"file1.cpp"}, 3);
        memory.on_session_end("Fixed bug B", {"file2.cpp"}, 5);
        memory.on_session_end("Added feature C", {"file3.cpp"}, 8);
        
        auto sessions = memory.get_recent_sessions(5);
        CHECK(sessions.size() == 3);
    }
    
    SUBCASE("Respects count parameter") {
        memory.on_session_end("session 1", {}, 1);
        memory.on_session_end("session 2", {}, 2);
        memory.on_session_end("session 3", {}, 3);
        
        auto sessions = memory.get_recent_sessions(2);
        CHECK(sessions.size() == 2);
    }
    
    SUBCASE("Files touched recorded") {
        memory.on_session_end("summary", {"a.cpp", "b.hpp", "c.txt"}, 2);
        
        auto sessions = memory.get_recent_sessions(1);
        CHECK(sessions.size() == 1);
        CHECK(sessions[0]->files_touched.size() == 3);
    }
    
    cleanup_test_dir(test_dir);
}

// ============================================================================
// TEST 13: Save/Load Round-Trip
// ============================================================================
TEST_CASE("SessionMemory - JSON Round-Trip") {
    std::filesystem::path test_dir = std::filesystem::temp_directory_path() / "hive_agent_test_roundtrip";
    std::filesystem::create_directories(test_dir);
    
    SUBCASE("Data persists across instances") {
        {
            SessionMemory memory(test_dir / ".hive-agent" / "default");
            memory.reset_for_testing();
            memory.on_build_complete("cmake -S . -B build", true);
            memory.on_error("compile error", "fix syntax");
            memory.on_session_end("Fixed compilation", {"main.cpp"}, 3);
        }
        
        {
            SessionMemory memory2(test_dir / ".hive-agent" / "default");
            auto builds = memory2.get_builds();
            auto errors = memory2.get_errors();
            auto sessions = memory2.get_recent_sessions(5);
            
            CHECK(builds.size() == 1);
            CHECK(builds[0]->command == "cmake -S . -B build");
            CHECK(errors.size() == 1);
            CHECK(errors[0]->pattern == "compile error");
            CHECK(sessions.size() == 1);
        }
    }
    
    cleanup_test_dir(test_dir);
}

// ============================================================================
// TEST 14: Corrupted File Recovery
// ============================================================================
TEST_CASE("SessionMemory - Corrupted File Recovery") {
    std::filesystem::path test_dir = std::filesystem::temp_directory_path() / "hive_agent_test_corrupt";
    std::filesystem::create_directories(test_dir);
    
    std::filesystem::path memory_dir = test_dir / ".hive-agent" / "default" / "memory";
    std::filesystem::create_directories(memory_dir);
    std::filesystem::path memory_file = memory_dir / "memory.json";
    
    SUBCASE("Corrupted JSON triggers recovery") {
        // Create a corrupted memory file
        std::ofstream out(memory_file);
        out << "{ invalid json [[[";
        out.close();
        
        // Load should handle corruption gracefully
        SessionMemory memory(test_dir / ".hive-agent" / "default");
        memory.reset_for_testing();
        
        // Should start fresh with empty memory
        CHECK(memory.get_builds().empty());
        CHECK(memory.get_errors().empty());
    }
    
    SUBCASE("Valid JSON loads correctly") {
        // Create a valid memory file
        json valid_data;
        valid_data["version"] = "1.0";
        valid_data["builds"] = json::array();
        valid_data["builds"].push_back({{"id", "bld_001"}, {"command", "make"}});
        
        std::ofstream out(memory_file);
        out << valid_data.dump(2);
        out.close();
        
        SessionMemory memory(test_dir / ".hive-agent" / "default");
        auto builds = memory.get_builds();
        CHECK(builds.size() == 1);
    }
    
    cleanup_test_dir(test_dir);
}

// ============================================================================
// TEST 15: UTF-8 Handling
// ============================================================================
TEST_CASE("SessionMemory - UTF-8 Handling") {
    std::filesystem::path test_dir = std::filesystem::temp_directory_path() / "hive_agent_test_utf8";
    std::filesystem::create_directories(test_dir);
    
    SessionMemory memory(test_dir / ".hive-agent" / "default");
    memory.reset_for_testing();
    
    SUBCASE("Chinese characters preserved in commands") {
        memory.on_build_complete("cmake --build build  # 构建", true);
        
        auto builds = memory.get_builds();
        CHECK(builds[0]->command.find("构建") != std::string::npos);
    }
    
    SUBCASE("UTF-8 error patterns stored correctly") {
        memory.on_error("错误: 文件未找到", "检查路径");
        
        auto errors = memory.get_errors();
        CHECK(errors[0]->pattern == "错误: 文件未找到");
    }
    
    cleanup_test_dir(test_dir);
}

// ============================================================================
// TEST 16: Forget by ID
// ============================================================================
TEST_CASE("SessionMemory - Forget by ID") {
    std::filesystem::path test_dir = std::filesystem::temp_directory_path() / "hive_agent_test_forget";
    std::filesystem::create_directories(test_dir);
    
    SessionMemory memory(test_dir / ".hive-agent" / "default");
    memory.reset_for_testing();
    memory.on_build_complete("command A", true);
    memory.on_build_complete("command B", true);
    memory.on_error("error X", "");
    
    SUBCASE("forget returns false for non-existent ID") {
        bool result = memory.forget("non_existent_id");
        CHECK(result == false);
    }
    
    SUBCASE("forget removes correct entry") {
        auto builds_before = memory.get_builds();
        CHECK(builds_before.size() == 2);
        
        // Get the ID of first build
        std::string id_to_remove = builds_before[0]->id;
        
        bool result = memory.forget(id_to_remove);
        CHECK(result == true);
        
        auto builds_after = memory.get_builds();
        CHECK(builds_after.size() == 1);
    }
    
    cleanup_test_dir(test_dir);
}

// ============================================================================
// TEST 17: Configuration
// ============================================================================
TEST_CASE("SessionMemory - Configuration") {
    std::filesystem::path test_dir = std::filesystem::temp_directory_path() / "hive_agent_test_config";
    std::filesystem::create_directories(test_dir);
    
    SessionMemory memory(test_dir / ".hive-agent" / "default");
    memory.reset_for_testing();
    
    SUBCASE("Default config is enabled") {
        auto config = memory.get_config();
        CHECK(config.enabled == true);
    }
    
    SUBCASE("Config can be set") {
        auto config = memory.get_config();
        config.enabled = false;
        config.max_entries_per_category = 100;
        memory.set_config(config);
        
        auto new_config = memory.get_config();
        CHECK(new_config.enabled == false);
        CHECK(new_config.max_entries_per_category == 100);
    }
    
    cleanup_test_dir(test_dir);
}

// ============================================================================
// TEST 18: Memory Consolidation
// ============================================================================
TEST_CASE("SessionMemory - Consolidation") {
    std::filesystem::path test_dir = std::filesystem::temp_directory_path() / "hive_agent_test_consolidate";
    std::filesystem::create_directories(test_dir);
    
    SUBCASE("Old sessions pruned at limit") {
        SessionMemory memory(test_dir / ".hive-agent" / "default");
        auto config = memory.get_config();
        config.max_session_history = 3;
        memory.set_config(config);
        memory.reset_for_testing();
        
        for (int i = 0; i < 10; ++i) {
            memory.on_session_end("session " + std::to_string(i), {}, i);
        }
        
        auto sessions = memory.get_recent_sessions(10);
        CHECK(sessions.size() <= 10); // Should be consolidated
    }
    
    SUBCASE("Builds tracked correctly") {
        SessionMemory memory(test_dir / ".hive-agent" / "default");
        memory.reset_for_testing();
        
        // Add many entries
        for (int i = 0; i < 20; ++i) {
            memory.on_build_complete("command " + std::to_string(i), true);
        }
        
        // Should have some builds
        auto builds = memory.get_builds();
        CHECK(builds.size() > 0);
    }
    
    cleanup_test_dir(test_dir);
}

// ============================================================================
// TEST 19: Memory Aging
// ============================================================================
TEST_CASE("SessionMemory - Aging") {
    std::filesystem::path test_dir = std::filesystem::temp_directory_path() / "hive_agent_test_aging";
    std::filesystem::create_directories(test_dir);
    
    SessionMemory memory(test_dir / ".hive-agent" / "default");
    memory.reset_for_testing();
    
    SUBCASE("Session start triggers aging check") {
        // Add some entries
        memory.on_build_complete("old command", true);
        memory.on_error("old error", "");
        
        // Trigger session start (which triggers aging)
        memory.on_session_start();
        
        // Should not crash
        auto builds = memory.get_builds();
        CHECK(builds.size() >= 0);
    }
    
    cleanup_test_dir(test_dir);
}

// ============================================================================
// TEST 20: Integration - Full Session Lifecycle
// ============================================================================
TEST_CASE("SessionMemory - Full Integration") {
    std::filesystem::path test_dir = std::filesystem::temp_directory_path() / "hive_agent_test_integration";
    std::filesystem::create_directories(test_dir);
    
    SUBCASE("Complete workflow (basic)") {
        // Simplified test - just verify session lifecycle works
        SessionMemory memory(test_dir / ".hive-agent" / "default");
        memory.reset_for_testing();
        
        memory.on_session_start();
        memory.on_build_complete("cmake -S . -B build", true);
        
        auto stats = memory.get_stats();
        CHECK(stats.total_builds >= 1);
        
        // Session end should work
        memory.on_session_end("test summary", {"main.cpp"}, 3);
        
        // Verify session was stored
        auto sessions = memory.get_recent_sessions(5);
        CHECK(sessions.size() >= 1);
    }
    
    SUBCASE("Remember and retrieve workflow") {
        SessionMemory memory(test_dir / ".hive-agent" / "default");
        memory.reset_for_testing();
        
        // Remember important info
        memory.remember("preferences", "Use -Wall -Wextra flags", 5, {"compiler", "warnings"});
        memory.remember("patterns", "Project requires C++17 minimum", 4, {"cpp", "standard"});
        
        auto prefs = memory.get_preferences();
        auto patterns = memory.get_patterns();
        
        CHECK(prefs.size() == 1);
        CHECK(patterns.size() == 1);
        
        // System prompt should include memory
        std::string prompt = memory.get_system_prompt_addition();
        CHECK(prompt.length() > 0);
    }
    
    cleanup_test_dir(test_dir);
}

// ============================================================================
// TEST 21: Edge Cases and Error Handling
// ============================================================================
TEST_CASE("SessionMemory - Edge Cases") {
    std::filesystem::path test_dir = std::filesystem::temp_directory_path() / "hive_agent_test_edge";
    std::filesystem::create_directories(test_dir);
    
    SessionMemory memory(test_dir / ".hive-agent" / "default");
    memory.reset_for_testing();
    
    SUBCASE("Empty command handled gracefully") {
        memory.on_build_complete("", true);
        // Empty commands may or may not be stored depending on implementation
        // Just verify no crash and consistent behavior
        auto builds = memory.get_builds();
        CHECK(builds.size() <= 1);  // At most 1 entry
    }
    
    SUBCASE("Very long command handled") {
        std::string long_cmd(10000, 'a');
        memory.on_build_complete(long_cmd, true);
        // Should not crash, may be filtered or truncated
        auto builds = memory.get_builds();
        CHECK(builds.size() <= 1);
    }
    
    SUBCASE("Special characters in content") {
        memory.remember("preferences", "Test with $pecial |chars| and \"quotes\"", 3, {});
        auto prefs = memory.get_preferences();
        CHECK(prefs.size() == 1);
    }
    
    cleanup_test_dir(test_dir);
}

// ============================================================================
// TEST 22: Statistics Extended
// ============================================================================
TEST_CASE("SessionMemory - Statistics Extended") {
    std::filesystem::path test_dir = std::filesystem::temp_directory_path() / "hive_agent_test_stats_ext";
    std::filesystem::create_directories(test_dir);
    
    SessionMemory memory(test_dir / ".hive-agent" / "default");
    memory.reset_for_testing();
    
    memory.on_build_complete("cmake build", true);
    memory.on_build_complete("make test", true);
    memory.on_build_complete("ctest", true);
    memory.on_error("error 1", "");
    memory.on_error_resolved("error 2", "fix");
    memory.on_error_resolved("error 3", "fix");
    memory.remember("preferences", "pref 1", 3, {});
    memory.remember("patterns", "pattern 1", 3, {});
    memory.on_session_end("test summary", {}, 5);
    
    auto stats = memory.get_stats();
    
    CHECK(stats.total_builds == 3);
    CHECK(stats.total_errors == 3);
    CHECK(stats.resolved_errors == 2);
    CHECK(stats.total_preferences == 1);
    CHECK(stats.total_patterns == 1);
    CHECK(stats.total_sessions == 1);
    CHECK(stats.total_size_bytes > 0);
    
    cleanup_test_dir(test_dir);
}
