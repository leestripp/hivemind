#include "StarterSkills.h"

#include <system_error>

namespace fs = std::filesystem;

namespace hwskills {

bool valid_name(const std::string& skill) {
    if (skill.empty() || skill.size() > 64) return false;
    for (const char c : skill)
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-'))
            return false;
    return true;
}

std::vector<fs::path> workspace_roots(const fs::path& worker_base,
                                      const std::string& host_workspace_root) {
    std::vector<fs::path> roots{
        (worker_base / "workspace/.agents/skills").lexically_normal()};
    if (!host_workspace_root.empty()) {
        const fs::path host =
            (fs::path(host_workspace_root) / ".agents/skills").lexically_normal();
        if (host != roots.front()) roots.push_back(host);
    }
    return roots;
}

void discard_partial(const InstallPlan& plan) {
    for (const auto& dir : plan.dirs) {
        std::error_code ec;
        fs::remove_all(dir, ec);
    }
}

InstallPlan prepare_install(const std::vector<fs::path>& roots,
                            const std::string& skill) {
    InstallPlan plan;
    if (!valid_name(skill)) {
        plan.error = "bad skill name";
        return plan;
    }
    std::error_code ec;
    for (const auto& root : roots) {
        const fs::path dest = root / skill;
        if (fs::exists(dest, ec)) continue;
        ec.clear();
        fs::create_directories(dest, ec);
        if (ec) {
            discard_partial(plan);
            plan.dirs.clear();
            plan.error = "cannot create workspace skill: " + ec.message();
            return plan;
        }
        plan.dirs.push_back(dest);
    }
    return plan;
}

std::string remove_starter(const std::vector<fs::path>& roots,
                           const std::string& skill) {
    if (!valid_name(skill)) return "bad skill name";
    std::error_code first;
    for (const auto& root : roots) {
        std::error_code ec;
        fs::remove_all(root / skill, ec);
        if (ec && !first) first = ec;
    }
    return first ? first.message() : "";
}

void make_agent_writable(const fs::path& root) {
    std::error_code ec;
    fs::permissions(root, fs::perms::all, fs::perm_options::replace, ec);
    const auto rw = fs::perms::owner_read | fs::perms::owner_write |
                    fs::perms::group_read | fs::perms::group_write |
                    fs::perms::others_read | fs::perms::others_write;
    for (auto it = fs::recursive_directory_iterator(root, ec);
         !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (it->is_directory(ec))
            fs::permissions(it->path(), fs::perms::all,
                            fs::perm_options::replace, ec);
        else if (it->is_regular_file(ec))
            fs::permissions(it->path(), rw, fs::perm_options::replace, ec);
    }
}

}  // namespace hwskills
