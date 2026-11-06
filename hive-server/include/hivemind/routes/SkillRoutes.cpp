// include/hivemind/routes/SkillRoutes.cpp
// Curated starter-skill installer. The hub stores only the small curated
// catalog's installed source copies; agents receive writable workspace-global
// copies. Personal skills and arbitrary workspace skills are not inventoried.
#include "hivemind/routes/SkillRoutes.h"
#include "hivemind/routes/RouteUtil.h"
#include "hivemind/AgentStore.h"
#include "hivemind/WorkerChannel.h"
#include "hivemind/WorkerStore.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <optional>
#include <string>
#include <system_error>
#include <vector>

using json = nlohmann::json;
namespace fs = std::filesystem;

namespace {
using hivemind::routes::path_segments;
using hivemind::routes::send_error;
using hivemind::routes::send_json;

// Internal distribution cache, not a skill directory exposed to agents.
fs::path starter_cache_root() {
    return AgentStore::agents_base_dir().parent_path() / "skills";
}

fs::path find_registry_file() {
    if (const char* pwd = std::getenv("PWD")) {
        const fs::path p = fs::path(pwd) / "web" / "dist" /
                           "skills-registry.json";
        if (fs::exists(p)) return p;
    }
    const fs::path p = fs::current_path() / "web" / "dist" /
                       "skills-registry.json";
    if (fs::exists(p)) return p;
    return fs::path("web/dist/skills-registry.json");
}

json load_registry() {
    std::ifstream f(find_registry_file());
    if (!f) return json{{"entries", json::array()}};
    json reg = json::parse(f, nullptr, false);
    if (!reg.is_object()) return json{{"entries", json::array()}};
    if (!reg.contains("entries") || !reg["entries"].is_array())
        reg["entries"] = json::array();
    return reg;
}

std::optional<json> registry_entry(const std::string& name) {
    for (const auto& entry : load_registry()["entries"]) {
        if (entry.value("name", "") == name)
            return std::optional<json>(entry);
    }
    return std::nullopt;
}

bool valid_skill_name(const std::string& name) {
    return !name.empty() && name.size() <= 64 &&
           std::ranges::all_of(name, [](unsigned char c) {
               return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                      c == '-';
           });
}

bool is_installed(const std::string& name) {
    std::error_code ec;
    const fs::path dir = starter_cache_root() / name;
    return fs::exists(dir / "SKILL.md", ec) && !fs::is_symlink(dir, ec);
}

json frontmatter_of(const fs::path& skill_md) {
    json fm = json::object();
    std::ifstream f(skill_md);
    if (!f) return fm;
    std::string line;
    bool in_fm = false;
    while (std::getline(f, line)) {
        if (line == "---") {
            if (in_fm) break;
            in_fm = true;
            continue;
        }
        if (!in_fm) continue;
        const auto pos = line.find(':');
        if (pos == std::string::npos) continue;
        const std::string key = line.substr(0, pos);
        std::string val = line.substr(pos + 1);
        while (!val.empty() && val.front() == ' ') val.erase(val.begin());
        while (!val.empty() && (val.back() == ' ' || val.back() == '\r'))
            val.pop_back();
        if (key == "name" || key == "description" || key == "version")
            fm[key] = val;
    }
    return fm;
}

std::vector<std::string> enrolled_workers(WorkerStore& workers) {
    std::vector<std::string> out;
    for (const auto& worker : workers.list()) out.push_back(worker.name);
    return out;
}

std::mutex starter_mutex;

json install_on_connected_workers(WorkerChannel& channel,
                                  const std::string& name) {
    json results = json::array();
    for (const auto& worker : channel.connected_workers()) {
        auto result = channel.push_skill(worker, name);
        json row = {{"worker", worker}};
        if (!result) {
            row["ok"] = false;
            row["error"] = "worker went offline";
        } else {
            row["ok"] = result->ok;
            row["files"] = result->files;
            row["bytes"] = result->bytes;
            if (!result->error.empty()) row["error"] = result->error;
        }
        results.push_back(std::move(row));
    }
    return results;
}

bool safe_git_field(const std::string& value, size_t max) {
    if (value.empty() || value.size() > max) return false;
    for (const unsigned char c : value) {
        if (std::isalnum(c)) continue;
        if (std::string(":/.-_@~%+").find(static_cast<char>(c)) !=
            std::string::npos)
            continue;
        return false;
    }
    return true;
}

}  // namespace

void sync_installed_starter_skills(WorkerChannel& channel,
                                   const std::string& worker) {
    for (const auto& entry : load_registry()["entries"]) {
        const std::string name = entry.value("name", "");
        if (!valid_skill_name(name) || !is_installed(name)) continue;
        auto result = channel.push_skill(worker, name);
        if (!result || !result->ok) {
            std::cerr << "[skills] starter sync failed for " << name << " on "
                      << worker << ": "
                      << (result ? result->error : "worker offline") << '\n';
        }
    }
}

void register_skill_routes(OmniNet::HTTP::Server& server, WorkerStore& workers,
                           WorkerChannel& channel) {
    server.get("/api/skills",
        [](const OmniNet::HTTP::Request&, OmniNet::HTTP::Response& resp) {
            json installed = json::array();
            for (auto entry : load_registry()["entries"]) {
                const std::string name = entry.value("name", "");
                if (!is_installed(name)) continue;
                entry["installed"] = true;
                installed.push_back(std::move(entry));
            }
            send_json(resp, json{{"skills", installed}});
        });

    server.get("/api/skills/registry",
        [](const OmniNet::HTTP::Request&, OmniNet::HTTP::Response& resp) {
            json reg = load_registry();
            for (auto& entry : reg["entries"])
                entry["installed"] = is_installed(entry.value("name", ""));
            send_json(resp, reg);
        });

    // Install only by curated catalog name. Arbitrary Git URLs and per-agent
    // enablement intentionally do not belong in this small installer API.
    server.post("/api/skills/install",
        [&workers, &channel](const OmniNet::HTTP::Request& req,
                             OmniNet::HTTP::Response& resp) {
            std::lock_guard lock(starter_mutex);
            json body;
            try {
                body = json::parse(req.bodyAsString());
            } catch (...) {
                send_error(resp, 400, "invalid JSON");
                return;
            }
            const std::string name = body.value("name", "");
            const auto entry = registry_entry(name);
            if (!entry) {
                send_error(resp, 400, "unknown starter skill");
                return;
            }
            if (is_installed(name)) {
                send_error(resp, 409, "skill '" + name + "' already installed");
                return;
            }

            const std::string url = entry->value("git_url", "");
            const std::string sub = entry->value("path", "");
            if (!safe_git_field(url, 300) ||
                (!sub.empty() &&
                 (!safe_git_field(sub, 120) || fs::path(sub).is_absolute() ||
                  sub.find("..") != std::string::npos))) {
                send_error(resp, 500, "curated skill has an invalid source");
                return;
            }

            char tmpl[] = "/tmp/hive-skill-XXXXXX";
            if (!mkdtemp(tmpl)) {
                send_error(resp, 500, "tmp dir failed");
                return;
            }
            const fs::path tmp = tmpl;
            struct Cleanup {
                fs::path p;
                ~Cleanup() {
                    std::error_code ec;
                    fs::remove_all(p, ec);
                    fs::remove(p.string() + ".log", ec);
                }
            } cleanup{tmp};

            const std::string cmd = "timeout 90 git clone --depth 1 '" + url +
                                    "' '" + tmp.string() + "' >'" +
                                    tmp.string() + ".log' 2>&1";
            if (std::system(cmd.c_str()) != 0) {
                std::ifstream log(tmp.string() + ".log");
                std::string tail, line;
                while (std::getline(log, line)) tail = line;
                send_error(resp, 502, "git clone failed: " + tail);
                return;
            }

            const fs::path src = sub.empty() ? tmp : tmp / sub;
            std::error_code ec;
            const json fm = frontmatter_of(src / "SKILL.md");
            if (!fs::exists(src / "SKILL.md", ec) ||
                fm.value("name", "") != name) {
                send_error(resp, 500, "curated source failed SKILL.md validation");
                return;
            }
            const fs::path dest = starter_cache_root() / name;
            const fs::path staging = starter_cache_root() /
                                     ("." + name + ".installing");
            fs::create_directories(starter_cache_root(), ec);
            fs::remove_all(staging, ec);
            ec.clear();
            fs::copy(src, staging, fs::copy_options::recursive, ec);
            if (!ec) fs::rename(staging, dest, ec);
            if (ec) {
                std::error_code cleanup;
                fs::remove_all(staging, cleanup);
                send_error(resp, 500, "copy failed: " + ec.message());
                return;
            }

            const json worker_results = install_on_connected_workers(channel, name);
            json offline = json::array();
            for (const auto& worker : enrolled_workers(workers))
                if (!channel.is_online(worker)) offline.push_back(worker);
            send_json(resp, json{{"installed", name},
                                 {"description", fm.value("description", "")},
                                 {"version", fm.value("version", "")},
                                 {"workers", worker_results},
                                 {"offline_workers", offline},
                                 {"offline_delivery", "automatic on reconnect"}}, 201);
        });

    server.delete_("/api/skills/:name",
        [&workers, &channel](const OmniNet::HTTP::Request& req,
                             OmniNet::HTTP::Response& resp) {
            std::lock_guard lock(starter_mutex);
            const auto seg = path_segments(req.path());
            if (seg.size() < 3 || !valid_skill_name(seg[2]) ||
                !registry_entry(seg[2])) {
                send_error(resp, 400, "unknown starter skill");
                return;
            }
            const std::string name = seg[2];
            const fs::path dir = starter_cache_root() / name;
            std::error_code ec;
            if (!fs::exists(dir, ec)) {
                send_error(resp, 404, "skill not installed");
                return;
            }
            json offline = json::array();
            for (const auto& worker : enrolled_workers(workers))
                if (!channel.is_online(worker)) offline.push_back(worker);
            if (!offline.empty()) {
                send_json(resp, json{{"error", "all enrolled workers must be online to uninstall"},
                                     {"offline_workers", offline}}, 409);
                return;
            }
            for (const auto& worker : channel.connected_workers()) {
                auto result = channel.remove_skill(worker, name);
                if (!result || !result->ok) {
                    send_error(resp, 500, "uninstall failed on " + worker + ": " +
                               (result ? result->error : "worker offline"));
                    return;
                }
            }
            fs::remove_all(dir, ec);
            if (ec) {
                send_error(resp, 500, "workspace copies removed but cache cleanup failed: " +
                           ec.message());
                return;
            }
            send_json(resp, json{{"deleted", name}});
        });
}
