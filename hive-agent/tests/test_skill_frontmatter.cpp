/**
 * @file test_skill_frontmatter.cpp
 * @brief Regression tests for the SKILL.md frontmatter parser
 *
 * Bug history:
 * - The previous implementation used std::regex with the pattern
 *   ^---\s*\n([\s\S]*?)\n---\s*\n?([\s\S]*)$ to extract YAML frontmatter
 *   from SKILL.md files. libstdc++'s std::regex implementation SEGFAULTS
 *   on large SKILL.md files (e.g. graphify, ~36 KB) that contain multiple
 *   '---' horizontal rules. This caused the engine to crash at startup when
 *   a large skill was installed in any discovery directory.
 *
 * The fix replaces the regex with a simple manual parser. These tests
 * verify the parser handles the cases that previously triggered the crash.
 */

#include <iostream>
#include <fstream>
#include <sstream>
#include <filesystem>
#include <string>
#include <cassert>

#include "core/SkillManager.h"

using namespace std;
using namespace std::filesystem;

static int failures = 0;
static int test_count = 0;

#define EXPECT(cond, msg) do { \
    ++test_count; \
    if (!(cond)) { \
        ++failures; \
        cerr << "  FAIL: " << msg << endl; \
    } else { \
        cerr << "  OK: " << msg << endl; \
    } \
} while (0)

// Test the frontmatter parser indirectly via load_skill on a temp directory.
// This is the same code path that crashed in production.
static void test_load_skill_with_large_skillmd() {
    cerr << "test_load_skill_with_large_skillmd" << endl;
    path tmp = temp_directory_path() / "hive_skill_frontmatter_test";
    remove_all(tmp);
    create_directories(tmp / "references");

    // Write a SKILL.md large enough to trigger the original std::regex crash
    // (graphify is ~36 KB; we use 40 KB to be safe)
    ofstream f(tmp / "SKILL.md");
    f << "---\n";
    f << "name: big\n";
    f << "description: \"A large skill to test frontmatter parsing\"\n";
    f << "---\n";
    f << "\n# Big Skill\n";
    // Pad with horizontal rules (---) to trigger the original crash
    for (int i = 0; i < 800; ++i) {
        f << "\n## Section " << i << "\n\n";
        f << "Some content for section " << i << ". Lorem ipsum dolor sit amet.\n";
        if (i % 50 == 0) {
            f << "\n---\n\n";  // Horizontal rule - causes std::regex to misbehave
        }
    }
    f.close();

    auto fsize = file_size(tmp / "SKILL.md");
    EXPECT(fsize > 30000, "SKILL.md is over 30 KB (was " + to_string(fsize) + ")");

    // create_instance removed in Phase 2 — get_instance() lazily creates.;
    auto* sm = SkillManager::get_instance();
    auto opt = sm->load_skill(tmp);
    EXPECT(opt.has_value(), "load_skill returned a value (no crash)");
    if (opt) {
        EXPECT(opt->metadata.name == "big",
               "name parsed correctly: '" + opt->metadata.name + "'");
        EXPECT(opt->metadata.description.find("large skill") != string::npos,
               "description parsed correctly");
        EXPECT(!opt->instructions.empty(),
               "instructions are non-empty (size=" + to_string(opt->instructions.size()) + ")");
    }
    SkillManager::delete_default();
    remove_all(tmp);
}

// Test with the actual graphify skill if installed
static void test_load_skill_graphify() {
    cerr << "test_load_skill_graphify" << endl;
    path graphify = path(getenv("HOME")) / ".agents/skills/graphify";
    if (!exists(graphify)) {
        cerr << "  SKIP: " << graphify << " not installed" << endl;
        return;
    }

    // create_instance removed in Phase 2 — get_instance() lazily creates.;
    auto* sm = SkillManager::get_instance();
    auto opt = sm->load_skill(graphify);
    EXPECT(opt.has_value(), "graphify SKILL.md loaded (no crash on 36 KB file)");
    if (opt) {
        EXPECT(opt->metadata.name == "graphify",
               "graphify name parsed: '" + opt->metadata.name + "'");
    }
    SkillManager::delete_default();
}

// Test edge case: minimal valid frontmatter
static void test_minimal_frontmatter() {
    cerr << "test_minimal_frontmatter" << endl;
    path tmp = temp_directory_path() / "hive_skill_minimal_test";
    remove_all(tmp);
    create_directories(tmp);

    {
        ofstream f(tmp / "SKILL.md");
        f << "---\nname: minimal\ndescription: \"tiny\"\n---\n\n# Body\n";
    }

    // create_instance removed in Phase 2 — get_instance() lazily creates.;
    auto* sm = SkillManager::get_instance();
    auto opt = sm->load_skill(tmp);
    EXPECT(opt.has_value(), "minimal skill loaded");
    if (opt) {
        EXPECT(opt->metadata.name == "minimal", "minimal name correct");
    }
    SkillManager::delete_default();
    remove_all(tmp);
}

// Test edge case: no frontmatter (should fail gracefully)
static void test_no_frontmatter() {
    cerr << "test_no_frontmatter" << endl;
    path tmp = temp_directory_path() / "daedalus_skill_nofront_test";
    remove_all(tmp);
    create_directories(tmp);

    {
        ofstream f(tmp / "SKILL.md");
        f << "# Just a heading, no frontmatter\n\nSome text.\n";
    }

    // create_instance removed in Phase 2 — get_instance() lazily creates.;
    auto* sm = SkillManager::get_instance();
    auto opt = sm->load_skill(tmp);
    EXPECT(!opt.has_value(), "no-frontmatter skill rejected");
    SkillManager::delete_default();
    remove_all(tmp);
}

int main() {
    cerr << "=== Skill Frontmatter Parser Tests ===" << endl;
    test_load_skill_with_large_skillmd();
    test_load_skill_graphify();
    test_minimal_frontmatter();
    test_no_frontmatter();

    cerr << "\nResults: " << (test_count - failures) << "/" << test_count << " passed" << endl;
    return failures > 0 ? 1 : 0;
}