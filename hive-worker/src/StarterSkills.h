#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace hwskills {

struct InstallPlan {
    std::vector<std::filesystem::path> dirs;
    std::string error;
};

bool valid_name(const std::string& skill);
std::vector<std::filesystem::path> workspace_roots(
    const std::filesystem::path& worker_base,
    const std::string& host_workspace_root);

// Create only roots where this starter is absent. Existing directories are
// agent-owned and must be preserved, including edited starter installations.
InstallPlan prepare_install(
    const std::vector<std::filesystem::path>& roots,
    const std::string& skill);
void discard_partial(const InstallPlan& plan);
std::string remove_starter(
    const std::vector<std::filesystem::path>& roots,
    const std::string& skill);
void make_agent_writable(const std::filesystem::path& root);

}  // namespace hwskills
