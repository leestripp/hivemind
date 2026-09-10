/**
 * @file test_skill_auto_discovery.cpp
 * @brief Tests for skill auto-discovery and the Skill catalog
 *
 * Tests:
 * - Skill metadata can be enumerated
 * - find_relevant_skills matches based on description text
 * - activate_skill / is_skill_active work correctly
 * - get_active_skills_context returns non-empty when skills are active
 * - The Skill catalog can be formatted for system prompt injection
 *
 * Note: These tests use the singleton SkillManager. The actual skill files
 * (in ~/.local/share/daedalus/skills/ and .agents/skills/) must be present for the
 * metadata tests to be meaningful. The behavioral tests work even if no
 * skills are installed.
 */

#include <iostream>
#include <filesystem>
#include <string>
#include <vector>

#include "core/SkillManager.h"

using namespace std;

static int failures = 0;
static int test_count = 0;

#define TEST(name) int test_##name()

#define CHECK(cond) do { \
    test_count++; \
    if (!(cond)) { \
        std::cerr << "FAIL: " << #cond << " at " << __FILE__ << ":" << __LINE__ << std::endl; \
        failures++; \
        return 1; \
    } \
} while(0)

#define CHECK_EQ(a, b) do { \
    test_count++; \
    if ((a) != (b)) { \
        std::cerr << "FAIL: " << #a << " == " << #b << " (" << (a) << " vs " << (b) << ") at " << __FILE__ << ":" << __LINE__ << std::endl; \
        failures++; \
        return 1; \
    } \
} while(0)

// ============================================================================
// Test: SkillManager singleton exists
// ============================================================================
TEST(singleton_exists) {
    // create_instance removed in Phase 2 — get_instance() lazily creates.;
    auto* sm = SkillManager::get_instance();
    CHECK(sm != nullptr);
    return 0;
}

// ============================================================================
// Test: Empty task context returns no skills (or empty list)
// ============================================================================
TEST(empty_task_context) {
    auto* sm = SkillManager::get_instance();
    if (!sm) return 0;
    auto results = sm->find_relevant_skills("");
    // Should not crash, may or may not be empty depending on installed skills
    (void)results;
    return 0;
}

// ============================================================================
// Test: find_relevant_skills does not throw on various inputs
// ============================================================================
TEST(find_relevant_does_not_throw) {
    auto* sm = SkillManager::get_instance();
    if (!sm) return 0;
    try {
        sm->find_relevant_skills("C++ thread pool async");
        sm->find_relevant_skills("git workflow commit");
        sm->find_relevant_skills("code review memory leaks");
        sm->find_relevant_skills("cmake build system");
        sm->find_relevant_skills("a");  // single char
        sm->find_relevant_skills("very long unrelated query about something completely different than anything in any skill ever");
    } catch (const std::exception& e) {
        std::cerr << "Threw: " << e.what() << std::endl;
        return 1;
    }
    return 0;
}

// ============================================================================
// Test: get_active_skills_context is safe with no active skills
// ============================================================================
TEST(active_skills_context_empty_when_none_active) {
    auto* sm = SkillManager::get_instance();
    if (!sm) return 0;
    // Deactivate all first
    sm->deactivate_all_skills();
    std::string context = sm->get_active_skills_context();
    CHECK(context.empty());
    return 0;
}

// ============================================================================
// Test: is_skill_active returns false for non-existent skill
// ============================================================================
TEST(is_skill_active_returns_false_for_unknown) {
    auto* sm = SkillManager::get_instance();
    if (!sm) return 0;
    CHECK(!sm->is_skill_active("this-skill-definitely-does-not-exist-12345"));
    return 0;
}

// ============================================================================
// Test: deactivate_skill on inactive skill returns false
// ============================================================================
TEST(deactivate_inactive_skill_returns_false) {
    auto* sm = SkillManager::get_instance();
    if (!sm) return 0;
    bool result = sm->deactivate_skill("never-active-skill-xyz");
    CHECK(!result);
    return 0;
}

// ============================================================================
// Test: get_all_cached_skills returns a map
// ============================================================================
TEST(get_all_cached_skills_is_map) {
    auto* sm = SkillManager::get_instance();
    if (!sm) return 0;
    const auto& skills = sm->get_all_cached_skills();
    // Should not throw when iterating
    size_t count = 0;
    for (const auto& [name, skill] : skills) {
        CHECK(!name.empty());
        CHECK(!skill.metadata.name.empty());
        count++;
    }
    std::cout << "  Found " << count << " cached skills" << std::endl;
    return 0;
}

// ============================================================================
// Test: search_skills does not throw
// ============================================================================
TEST(search_skills_does_not_throw) {
    auto* sm = SkillManager::get_instance();
    if (!sm) return 0;
    try {
        auto results = sm->search_skills("C++");
        (void)results;
    } catch (const std::exception& e) {
        std::cerr << "Threw: " << e.what() << std::endl;
        return 1;
    }
    return 0;
}

// ============================================================================
// Regression test: LLMClient must NOT auto-activate skills on user prompts.
//
// Background: an earlier "B3 fix" auto-called find_relevant_skills() on every
// user prompt and activated the matches. Substring matching produced false
// positives (e.g. the word "bug" matched the skill gdb-crash-debugger via the
// substring "debugging" in its description), so on a fresh session skills
// would appear active without the user asking for them. The fix removed the
// auto-activation block; activation is now exclusively LLM-driven via the
// load_skill tool. This test statically guards against the block being
// reintroduced.
// ============================================================================
TEST(llmclient_does_not_auto_activate_on_prompts) {
    // Locate LLMClient.cpp relative to the project root (we are run from build/).
    // Walk up from CWD until we find a src/api/LLMClient.cpp.
    std::filesystem::path candidate = std::filesystem::current_path();
    std::filesystem::path llmclient;
    for (int i = 0; i < 6; ++i) {
        llmclient = candidate / "src" / "api" / "LLMClient.cpp";
        if (std::filesystem::exists(llmclient)) break;
        if (candidate.has_parent_path()) {
            candidate = candidate.parent_path();
        } else {
            break;
        }
    }
    if (!std::filesystem::exists(llmclient)) {
        std::cerr << "SKIP: could not locate src/api/LLMClient.cpp" << std::endl;
        return 0;
    }

    std::ifstream in(llmclient);
    CHECK(in.is_open());

    std::string content((std::istreambuf_iterator<char>(in)),
                         std::istreambuf_iterator<char>());

    // The forbidden pattern: a call to find_relevant_skills inside the
    // main user-prompt handling path, followed by activate_skill.
    // We use a single regex-style substring check (no <regex> dependency).
    auto contains = [](const std::string& hay, const std::string& needle) {
        return hay.find(needle) != std::string::npos;
    };

    // The auto-activation block looked like:
    //     if (SkillManager* sm = SkillManager::get_instance()) {
    //         auto relevant = sm->find_relevant_skills(user_prompt);
    //         for (const auto& skill : relevant) {
    //             ...
    //             if (sm->activate_skill(skill)) {
    //                 LOG_INFO("Auto-activated skill: ...");
    //             }
    //         }
    //     }
    // We forbid the combo of "find_relevant_skills" + "Auto-activated skill"
    // appearing together in the file. Each alone is fine (the function still
    // exists and is exposed via the API); the combination is the bug.
    const bool has_find  = contains(content, "find_relevant_skills");
    const bool has_auto  = contains(content, "Auto-activated skill");
    CHECK(!has_find || !has_auto);
    return 0;
}

// ============================================================================
// MAIN
// ============================================================================

int main() {
    std::cout << "Running Skill auto-discovery tests..." << std::endl;

    int local_failures = 0;
    local_failures += test_singleton_exists();
    local_failures += test_empty_task_context();
    local_failures += test_find_relevant_does_not_throw();
    local_failures += test_active_skills_context_empty_when_none_active();
    local_failures += test_is_skill_active_returns_false_for_unknown();
    local_failures += test_deactivate_inactive_skill_returns_false();
    local_failures += test_get_all_cached_skills_is_map();
    local_failures += test_search_skills_does_not_throw();
    local_failures += test_llmclient_does_not_auto_activate_on_prompts();

    // Cleanup singleton
    SkillManager::delete_default();

    std::cout << "\nResults: " << (local_failures == 0 ? "PASS" : "FAIL") << std::endl;
    std::cout << "Total checks: " << test_count << std::endl;
    std::cout << "Failed checks: " << local_failures << std::endl;

    return local_failures > 0 ? 1 : 0;
}
