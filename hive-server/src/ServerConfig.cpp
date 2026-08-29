// =============================================================================
// src/ServerConfig.cpp — server-level settings (workspace config)
// =============================================================================
#include "hivemind/ServerConfig.h"

#include <regex>
#include <nlohmann/json.hpp>

#include <cstdlib>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace fs = std::filesystem;

ServerConfig& ServerConfig::instance() {
    static ServerConfig cfg{
        fs::path{std::getenv("HOME") ? std::getenv("HOME") : "."}
        / ".local" / "share" / "hivemind" / "server.json"};
    return cfg;
}

ServerConfig::ServerConfig(fs::path file) : file_(std::move(file)) {
    load();
}

std::string ServerConfig::expand_home(const std::string& path) {
    if (path == "~" || path.rfind("~/", 0) == 0) {
        const char* home = std::getenv("HOME");
        if (home && *home)
            return std::string{home} + path.substr(1);
    }
    return path;
}

std::string ServerConfig::default_workspace_root() {
    return (fs::path{std::getenv("HOME") ? std::getenv("HOME") : "."}
            / ".local" / "share" / "hivemind" / "workspace").string();
}

void ServerConfig::load() {
    std::ifstream f{file_};
    if (!f) return;
    try {
        auto j = nlohmann::json::parse(f);
        workspace_root_ = j.value("workspace_root", "");
        warm_containers_ = j.value("warm_containers", true);
        screen_resolution_ = j.value("screen_resolution", "1280x800");
        worker_ws_port_ = j.value("worker_ws_port", 8423);
        mcp_bridge_port_ = j.value("mcp_bridge_port", 8424);
        worker_ws_bind_ = j.value("worker_ws_bind", std::string{"0.0.0.0"});
        cors_origins_.clear();
        if (j.contains("cors_origins") && j["cors_origins"].is_array())
            for (auto& o : j["cors_origins"])
                if (o.is_string()) cors_origins_.push_back(o.get<std::string>());
    } catch (...) { /* corrupt file → defaults */ }
}

void ServerConfig::save() const {
    std::error_code ec;
    fs::create_directories(file_.parent_path(), ec);
    std::ofstream f{file_, std::ios::trunc};
    if (!f) throw std::runtime_error("cannot write " + file_.string());
    f << nlohmann::json{{"workspace_root", workspace_root_},
                        {"cors_origins", cors_origins_},
                        {"warm_containers", warm_containers_},
                        {"screen_resolution", screen_resolution_}}.dump(2) << "\n";
}

std::string ServerConfig::workspace_root() {
    std::string root = workspace_root_.empty()
                       ? default_workspace_root()
                       : expand_home(workspace_root_);
    std::error_code ec;
    fs::create_directories(root, ec);
    seed_workspace(fs::path{root});
    return root;
}

// First-run seeding of the shared company drive from the in-repo
// skeleton (hive-server/data/workspace/): index.md is the map every
// containerized agent is told to read before file work (AgentHost
// system prompt). Never overwrites — only runs when index.md is
// missing, so user content and custom layouts are safe.
void ServerConfig::seed_workspace(const fs::path& root) {
    std::error_code ec;
    if (fs::exists(root / "index.md", ec)) return;

    // The skeleton ships in the repo. Find it relative to the CWD
    // (the systemd unit runs from the checkout) or the executable.
    fs::path skel;
    for (const fs::path& cand : {fs::path{"hive-server/data/workspace"},
                                 fs::path{"/proc/self/exe"}.parent_path()
                                         .parent_path().parent_path()
                                 / "hive-server" / "data" / "workspace"}) {
        if (fs::exists(cand / "index.md", ec)) { skel = cand; break; }
    }
    if (skel.empty()) {
        std::cerr << "[config] workspace skeleton not found "
                     "(hive-server/data/workspace) — starting empty\n";
        return;
    }
    fs::copy(skel, root,
             fs::copy_options::recursive | fs::copy_options::skip_existing,
             ec);
    // Empty skeleton dirs that git can't track:
    for (const char* d : {"projects", "inbox", "attachments",
                          "scratch", "archive"})
        fs::create_directories(root / d, ec);
    std::cerr << "[config] seeded company workspace " << root
              << " from " << skel << "\n";
}

void ServerConfig::set_workspace_root(const std::string& path) {
    std::string expanded = expand_home(path);
    if (expanded.empty() || expanded.front() != '/')
        throw std::invalid_argument(
            "workspace_root must be an absolute path (got '" + path + "')");
    std::error_code ec;
    fs::create_directories(expanded, ec);
    if (ec)
        throw std::invalid_argument("cannot create '" + expanded + "': " +
                                    ec.message());
    if (!fs::is_directory(expanded))
        throw std::invalid_argument("'" + expanded + "' is not a directory");
    workspace_root_ = expanded;
    save();
}

void ServerConfig::set_screen_resolution(const std::string& res) {
    // WxH, 800x600 .. 7680x4320 — Xvfb rejects nonsense; fail early here.
    static const std::regex re("^([0-9]{3,4})x([0-9]{3,4})$");
    std::smatch m;
    if (!std::regex_match(res, m, re) ||
        std::stoi(m[1]) < 800 || std::stoi(m[1]) > 7680 ||
        std::stoi(m[2]) < 600 || std::stoi(m[2]) > 4320)
        throw std::invalid_argument(
            "screen_resolution must be WxH between 800x600 and 7680x4320");
    screen_resolution_ = res;
    save();
}

void ServerConfig::set_warm_containers(bool on) {
    warm_containers_ = on;
    save();
}

void ServerConfig::set_cors_origins(std::vector<std::string> origins) {
    cors_origins_ = std::move(origins);
    save();
}

bool ServerConfig::cors_allowed(const std::string& origin) const {
    for (auto& o : cors_origins_)
        if (o == "*" || o == origin) return true;
    return false;
}
