// test_workspace_seed.cpp — hwrunner::seed_workspace (first-run seeding).
//
// A fresh/reset worker gets only the global skill-library location. Startup
// must not recreate the historical department folders or overwrite content.

#include "../src/runner.h"

#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>

namespace fs = std::filesystem;

int main() {
    const fs::path root = fs::temp_directory_path() / "hive_workspace_seed_test";
    fs::remove_all(root);

    hwrunner::seed_workspace(root.string());
    assert(fs::is_directory(root / ".agents" / "skills"));
    for (const char* path : {"departments", "projects", "inbox", "attachments",
                             "scratch", "archive", "index.md"})
        assert(!fs::exists(root / path));

    // Startup is non-destructive and does not impose a fresh-install layout
    // on an existing agent-maintained workspace.
    {
        std::ofstream out(root / "index.md", std::ios::trunc);
        out << "# agent-maintained live map\n";
    }
    fs::create_directories(root / "projects" / "example");
    hwrunner::seed_workspace(root.string());
    assert(fs::is_regular_file(root / "index.md"));
    assert(fs::is_directory(root / "projects" / "example"));
    assert(fs::is_directory(root / ".agents" / "skills"));
    assert(!fs::exists(root / "departments"));

    fs::remove_all(root);
    std::cout << "test_workspace_seed OK\n";
    return 0;
}
