// ============================================================================
// tests/test_skill_lazy_discovery.cpp
//
// §Skills-Global (2026-09-30) regression tests.
//
// Field report (Chief, 2026-09-29): a skill dropped into the shared
// workspace global dir stayed invisible for ~18 minutes (catalog served
// the startup cache; refresh only happened via agent idle-respawn), two
// malformed skills were silently invisible for 15 HOURS (no skip
// reasons), and edits to an existing skill never reached load-by-name
// (cache-first).
//
// Fixes under test:
//   1. maybe_refresh_cache() rescans when stale, skips when fresh.
//   2. refresh_cache() picks up skills created AFTER initialization.
//   3. Skipped would-be skills are recorded with reasons.
//   4. load_skill_by_name() is disk-first: edits are picked up without
//      a refresh, and later (higher-priority) directories win collisions.
//   5. include_default_directories=false scans ONLY host-passed dirs.
// ============================================================================

#include "core/SkillManager.h"

#undef NDEBUG
#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <thread>

namespace fs = std::filesystem;

namespace {

fs::path fresh_dir(const std::string& tag) {
    fs::path d = fs::temp_directory_path() / ("skill-lazy-" + tag);
    fs::remove_all(d);
    fs::create_directories(d);
    return d;
}

void write_skill(const fs::path& dir, const std::string& name,
                 const std::string& description) {
    fs::create_directories(dir / name);
    std::ofstream f(dir / name / "SKILL.md");
    f << "---\nname: " << name << "\ndescription: " << description
      << "\n---\n\nbody\n";
}

// Configure a SkillManager scanning ONLY the given dirs (no env-dependent
// defaults). In-place: SkillManager has a std::mutex member, so it is
// neither copyable nor movable.
void make_manager(SkillManager& mgr, const std::vector<fs::path>& dirs) {
    mgr.include_default_directories = false;
    mgr.init_after_settings();
    for (const auto& d : dirs) mgr.add_skill_directory(d);
}

// 1+2: a skill created AFTER the initial discovery appears on refresh.
void test_lazy_pickup_on_refresh() {
    auto root = fresh_dir("refresh");
    auto dir = root / "global";
    fs::create_directories(dir);

    SkillManager mgr;
    make_manager(mgr, {dir});
    auto found = mgr.discover_all_skills();
    assert(found.empty());

    // Agent creates a global skill at runtime.
    write_skill(dir, "runtime-skill", "created after init");

    mgr.refresh_cache();  // live rescan
    auto cached = mgr.get_all_cached_skills();
    assert(cached.count("runtime-skill") == 1);
    assert(cached.at("runtime-skill").metadata.description == "created after init");
    std::cout << "test_lazy_pickup_on_refresh OK\n";
}

// 1: maybe_refresh_cache throttles.
void test_maybe_refresh_throttle() {
    auto root = fresh_dir("throttle");
    auto dir = root / "global";
    fs::create_directories(dir);
    write_skill(dir, "alpha", "alpha skill");

    SkillManager mgr;
    make_manager(mgr, {dir});
    mgr.discover_all_skills();  // stamps last_scan_

    // Fresh: no rescan.
    assert(!mgr.maybe_refresh_cache(std::chrono::seconds(3600)));
    // Stale (zero max age): rescans.
    assert(mgr.maybe_refresh_cache(std::chrono::seconds(0)));
    std::cout << "test_maybe_refresh_throttle OK\n";
}

// 3: malformed skills are recorded with skip reasons, not dropped silently.
void test_skip_reasons() {
    auto root = fresh_dir("skips");
    auto dir = root / "global";
    fs::create_directories(dir);

    write_skill(dir, "good", "good skill");
    fs::create_directories(dir / "no-skillmd");              // empty dir
    fs::create_directories(dir / "no-frontmatter");
    {
        std::ofstream f(dir / "no-frontmatter" / "SKILL.md");
        f << "just prose, no frontmatter\n";
    }
    fs::create_directories(dir / "no-description");
    {
        std::ofstream f(dir / "no-description" / "SKILL.md");
        f << "---\nname: no-description\n---\n\nbody\n";
    }

    SkillManager mgr;
    make_manager(mgr, {dir});
    mgr.refresh_cache();

    assert(mgr.get_all_cached_skills().count("good") == 1);
    const auto skipped = mgr.get_last_scan_skipped();
    assert(skipped.size() == 3);
    bool saw_no_skillmd = false, saw_no_frontmatter = false, saw_no_desc = false;
    for (const auto& sk : skipped) {
        const auto p = sk.path.filename().string();
        if (p == "no-skillmd") { saw_no_skillmd = true; assert(sk.reason.find("SKILL.md") != std::string::npos); }
        if (p == "no-frontmatter") { saw_no_frontmatter = true; assert(sk.reason.find("frontmatter") != std::string::npos); }
        if (p == "no-description") { saw_no_desc = true; assert(sk.reason.find("description") != std::string::npos); }
    }
    assert(saw_no_skillmd && saw_no_frontmatter && saw_no_desc);
    std::cout << "test_skip_reasons OK\n";
}

// 4a: load_skill_by_name is disk-first — edits apply without a refresh.
void test_load_by_name_sees_edits() {
    auto root = fresh_dir("edits");
    auto dir = root / "global";
    fs::create_directories(dir);
    write_skill(dir, "editable", "v1 description");

    SkillManager mgr;
    make_manager(mgr, {dir});
    mgr.refresh_cache();
    auto v1 = mgr.load_skill_by_name("editable");
    assert(v1 && v1->metadata.description == "v1 description");

    // Agent edits the skill at runtime; cache is now stale.
    write_skill(dir, "editable", "v2 description");
    auto v2 = mgr.load_skill_by_name("editable");
    assert(v2 && v2->metadata.description == "v2 description");
    std::cout << "test_load_by_name_sees_edits OK\n";
}

// 4b: collision priority — the LATER directory wins (workspace global is
// listed last so it shadows the read-only library).
void test_later_dir_wins() {
    auto root = fresh_dir("priority");
    auto library = root / "library";   // e.g. read-only /skills
    auto global  = root / "global";    // e.g. /workspace/.agents/skills
    write_skill(library, "shared-name", "library copy");
    write_skill(global,  "shared-name", "workspace copy");

    SkillManager mgr;
    make_manager(mgr, {library, global});
    mgr.refresh_cache();
    assert(mgr.get_all_cached_skills().at("shared-name").metadata.description
           == "workspace copy");
    auto loaded = mgr.load_skill_by_name("shared-name");
    assert(loaded && loaded->metadata.description == "workspace copy");
    std::cout << "test_later_dir_wins OK\n";
}

// 5: include_default_directories=false scans only host-passed dirs.
void test_replace_default_dirs() {
    auto root = fresh_dir("nodefaults");
    auto dir = root / "only";
    fs::create_directories(dir);

    SkillManager mgr;
    make_manager(mgr, {dir});
    const auto scanned = mgr.skill_directories();
    assert(scanned.size() == 1);
    assert(scanned[0] == dir);

    // Default (true) still adds the built-in dirs.
    SkillManager dflt;
    dflt.init_after_settings();
    assert(dflt.skill_directories().size() >= 2);
    std::cout << "test_replace_default_dirs OK\n";
}

}  // namespace

int main() {
    test_lazy_pickup_on_refresh();
    test_maybe_refresh_throttle();
    test_skip_reasons();
    test_load_by_name_sees_edits();
    test_later_dir_wins();
    test_replace_default_dirs();
    std::cout << "test_skill_lazy_discovery: all passed\n";
    return 0;
}
