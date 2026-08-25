// =============================================================================
// src/ServerConfig.cpp — server-level settings (workspace config)
// =============================================================================
#include "hivemind/ServerConfig.h"

#include <nlohmann/json.hpp>

#include <cstdlib>
#include <fstream>
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
                        {"warm_containers", warm_containers_}}.dump(2) << "\n";
}

std::string ServerConfig::workspace_root() {
    std::string root = workspace_root_.empty()
                       ? default_workspace_root()
                       : expand_home(workspace_root_);
    std::error_code ec;
    fs::create_directories(root, ec);
    return root;
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
