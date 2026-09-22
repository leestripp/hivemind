// test_workspace_seed.cpp — hwrunner::seed_workspace (first-run seeding).
//
// Verifies: layout dirs are created, index.md is written when absent,
// an existing index.md is NEVER overwritten, and reruns are idempotent.

#include "../src/runner.h"

#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

namespace fs = std::filesystem;

static std::string slurp(const fs::path& p) {
    std::ifstream in(p);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

int main() {
    const fs::path root = fs::temp_directory_path() / "hive_workspace_seed_test";
    fs::remove_all(root);   // clean slate

    // First run: creates the layout + the default map.
    hwrunner::seed_workspace(root.string());
    for (const char* d : {"departments/chief-of-staff", "departments/design",
                          "departments/engineering", "departments/executive",
                          "departments/marketing", "departments/research",
                          "projects", "inbox", "attachments", "scratch",
                          "archive"})
        assert(fs::is_directory(root / d));
    assert(fs::is_regular_file(root / "index.md"));
    const std::string seeded = slurp(root / "index.md");
    assert(seeded.find("NO sync") != std::string::npos);   // per-worker truth
    assert(seeded.find("departments/") != std::string::npos);

    // Second run with an agent-maintained map: must not clobber it.
    {
        std::ofstream out(root / "index.md", std::ios::trunc);
        out << "# agent-maintained live map\n";
    }
    hwrunner::seed_workspace(root.string());
    assert(slurp(root / "index.md") == "# agent-maintained live map\n");

    fs::remove_all(root);
    std::cout << "test_workspace_seed OK\n";
    return 0;
}
