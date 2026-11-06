#include "StarterSkills.h"

#undef NDEBUG
#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>

namespace fs = std::filesystem;

namespace {
std::string read(const fs::path& file) {
    std::ifstream in(file);
    return {std::istreambuf_iterator<char>(in),
            std::istreambuf_iterator<char>()};
}
}

int main() {
    assert(hwskills::valid_name("code-craft-cpp"));
    assert(!hwskills::valid_name(""));
    assert(!hwskills::valid_name("../private"));
    assert(!hwskills::valid_name("Bad_Name"));

    const fs::path base = fs::temp_directory_path() / "hive-starter-skills-test";
    fs::remove_all(base);
    const fs::path shared = base / "shared";
    const fs::path host = base / "host";
    const fs::path private_dir = base / "private";
    const std::vector<fs::path> roots{shared, host};

    // An existing edited copy is not part of the write plan and is untouched.
    fs::create_directories(shared / "sample");
    { std::ofstream(shared / "sample/SKILL.md") << "agent correction\n"; }
    auto plan = hwskills::prepare_install(roots, "sample");
    assert(plan.error.empty());
    assert(plan.dirs.size() == 1 && plan.dirs.front() == host / "sample");
    assert(read(shared / "sample/SKILL.md") == "agent correction\n");

    fs::create_directories(host / "sample");
    { std::ofstream(host / "sample/SKILL.md") << "starter copy\n"; }
    hwskills::make_agent_writable(host / "sample");
    const auto perms = fs::status(host / "sample/SKILL.md").permissions();
    assert((perms & fs::perms::others_write) != fs::perms::none);
    const auto root_perms = fs::status(host / "sample").permissions();
    assert((root_perms & fs::perms::others_write) != fs::perms::none);

    // A reconnect with the same starter writes nowhere and preserves both copies.
    auto again = hwskills::prepare_install(roots, "sample");
    assert(again.error.empty() && again.dirs.empty());
    assert(read(shared / "sample/SKILL.md") == "agent correction\n");
    assert(read(host / "sample/SKILL.md") == "starter copy\n");

    // An agent-created private skill with the same name is never uninstalled.
    fs::create_directories(private_dir / "sample");
    { std::ofstream(private_dir / "sample/SKILL.md") << "private\n"; }
    assert(hwskills::remove_starter(roots, "sample").empty());
    assert(!fs::exists(shared / "sample"));
    assert(!fs::exists(host / "sample"));
    assert(read(private_dir / "sample/SKILL.md") == "private\n");

    // Failed multi-root creation cleans only newly-created partial directories.
    const fs::path blocker = base / "blocker";
    { std::ofstream(blocker) << "not a directory"; }
    auto failed = hwskills::prepare_install({base / "partial", blocker}, "new-one");
    assert(!failed.error.empty() && failed.dirs.empty());
    assert(!fs::exists(base / "partial/new-one"));

    fs::remove_all(base);
    std::cout << "test_starter_skills: all passed\n";
}
