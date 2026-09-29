// include/hivemind/routes/SkillRoutes.cpp
// -----------------------------------------------------------------------------
// Global skills library + per-agent enable symlinks (see header).
// -----------------------------------------------------------------------------
#include "hivemind/routes/SkillRoutes.h"
#include "hivemind/AgentTools.h"
#include "hivemind/routes/RouteUtil.h"
#include "hivemind/AgentStore.h"
#include "hivemind/WorkerRouter.h"

#include <set>

#include <nlohmann/json.hpp>

#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

using json = nlohmann::json;
namespace fs = std::filesystem;

namespace {
using hivemind::routes::path_segments;
using hivemind::routes::send_json;
using hivemind::routes::send_error;

// ~/.local/share/hivemind/skills/ — the managed global library.
fs::path global_skills_root() {
    return AgentStore::agents_base_dir().parent_path() / "skills";
}

fs::path agent_skills_root(const std::string& agent_id) {
    return AgentStore::agents_base_dir() / agent_id / ".agents" / "skills";
}

bool valid_skill_name(const std::string& n) {
    if (n.empty() || n.size() > 64) return false;
    for (char c : n) {
        bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-';
        if (!ok) return false;
    }
    return true;
}

// web/dist/skills-registry.json — same discovery as StaticRoutes.
fs::path find_registry_file() {
    if (const char* pwd = std::getenv("PWD")) {
        fs::path p = fs::path(pwd) / "web" / "dist" / "skills-registry.json";
        if (fs::exists(p)) return p;
    }
    fs::path p = fs::current_path() / "web" / "dist" / "skills-registry.json";
    if (fs::exists(p)) return p;
    return fs::path("web/dist/skills-registry.json");
}

// Pull name/description/version out of SKILL.md YAML frontmatter (flat keys).
json frontmatter_of(const fs::path& skill_md) {
    json fm = json::object();
    std::ifstream f(skill_md);
    if (!f) return fm;
    std::string line;
    bool in_fm = false;
    while (std::getline(f, line)) {
        if (line == "---") { if (in_fm) break; in_fm = true; continue; }
        if (!in_fm) continue;
        auto pos = line.find(':');
        if (pos == std::string::npos) continue;
        std::string key = line.substr(0, pos);
        std::string val = line.substr(pos + 1);
        while (!val.empty() && val.front() == ' ') val.erase(val.begin());
        while (!val.empty() && (val.back() == ' ' || val.back() == '\r')) val.pop_back();
        if (key == "name" || key == "description" || key == "version")
            fm[key] = val;
    }
    return fm;
}

// A symlink whose target (relative or absolute) lands inside the global
// library. Agent-local skills are real directories; globals are links.
bool is_global_link(const fs::path& link) {
    std::error_code ec;
    if (!fs::is_symlink(link, ec)) return false;
    fs::path target = fs::read_symlink(link, ec);
    if (ec) return false;
    if (target.is_relative()) target = link.parent_path() / target;
    target = fs::weakly_canonical(target, ec);
    if (ec) return false;
    const auto root = fs::weakly_canonical(global_skills_root(), ec);
    if (ec) return false;
    auto rt = root.string();
    return target.string().starts_with(rt + "/");
}

std::vector<std::string> enabled_global_skills(const std::string& agent_id) {
    std::vector<std::string> out;
    std::error_code ec;
    fs::path root = agent_skills_root(agent_id);
    if (!fs::exists(root, ec)) return out;
    for (auto& e : fs::directory_iterator(root, ec))
        if (is_global_link(e.path()) && fs::exists(e.path() / "SKILL.md", ec))
            out.push_back(e.path().filename().string());
    return out;
}

} // namespace

void register_skill_routes(OmniNet::HTTP::Server& server,
                                             AgentStore& agents) {

    // ── GET /api/skills ───────────────────────────────────────────────────
    server.get("/api/skills",
        [&agents](const OmniNet::HTTP::Request&, OmniNet::HTTP::Response& resp) {
            std::error_code ec;
            fs::create_directories(global_skills_root(), ec);
            json list = json::array();
            auto all = agents.list_agents();
            for (auto& e : fs::directory_iterator(global_skills_root(), ec)) {
                if (!e.is_directory() || fs::is_symlink(e.path())) continue;
                if (!fs::exists(e.path() / "SKILL.md", ec)) continue;
                const std::string sname = e.path().filename().string();
                json fm = frontmatter_of(e.path() / "SKILL.md");
                json enabled = json::array();
                for (auto& a : all) {
                    auto eg = enabled_global_skills(a.id);
                    if (std::find(eg.begin(), eg.end(), sname) != eg.end())
                        enabled.push_back({{"id", a.id}, {"name", a.name}});
                }
                list.push_back({
                    {"name", sname},
                    {"description", fm.value("description", "")},
                    {"version", fm.value("version", "")},
                    {"enabled_agents", enabled},
                });
            }
            send_json(resp, json{{"skills", list}});
        });

    // ── GET /api/skills/registry — curated marketplace catalog (Phase 21) ──
    // Bundled JSON index of known-good skill repos; entries are annotated
    // with installed=true when the name is already in the global library.
    // §Skills-Global (2026-09-30): per-worker workspace global skills.
    // Relay-only — the hub asks each worker for its
    // /workspace/.agents/skills contents and persists nothing. Workers
    // are derived from the roster's worker field (machines that run
    // agents); offline workers report online:false with empty skills.
    server.get("/api/skills/workspace",
               [&agents](const OmniNet::HTTP::Request&,
                         OmniNet::HTTP::Response& res) {
        std::set<std::string> worker_names;
        for (const auto& a : agents.list_agents()) {
            worker_names.insert(a.worker.empty() ? "local" : a.worker);
        }
        json workers = json::array();
        for (const auto& w : worker_names) {
            json wj = {{"worker", w}};
            try {
                auto r = WorkerRouter::workspace_skills(w, 10);
                if (r) {
                    wj["online"] = true;
                    wj["skills"] = r->skills;
                } else {
                    wj["online"] = false;
                    wj["skills"] = json::array();
                }
            } catch (const std::exception& e) {
                wj["online"] = true;   // reachable, but the relay failed
                wj["skills"] = json::array();
                wj["error"] = e.what();
            }
            workers.push_back(std::move(wj));
        }
        send_json(res, {{"workers", workers}});
    });

    server.get("/api/skills/registry",
        [](const OmniNet::HTTP::Request&, OmniNet::HTTP::Response& resp) {
            std::ifstream f(find_registry_file());
            if (!f) { send_json(resp, json{{"entries", json::array()}}); return; }
            json reg = json::parse(f, nullptr, false);
            if (!reg.is_object()) {
                send_error(resp, 500, "registry file is not valid JSON"); return;
            }
            std::error_code ec;
            if (!reg.contains("entries") || !reg["entries"].is_array())
                reg["entries"] = json::array();
            for (auto& e : reg["entries"]) {   // by reference — value() copies!
                const std::string n = e.value("name", "");
                const bool inst = !n.empty()
                    && fs::exists(global_skills_root() / n / "SKILL.md", ec)
                    && !fs::is_symlink(global_skills_root() / n, ec);
                e["installed"] = inst;
            }
            send_json(resp, reg);
        });

    // ── POST /api/skills ──────────────────────────────────────────────────
    server.post("/api/skills",
        [](const OmniNet::HTTP::Request& req, OmniNet::HTTP::Response& resp) {
            json body;
            try { body = json::parse(req.bodyAsString()); }
            catch (...) { send_error(resp, 400, "invalid JSON"); return; }
            std::string name = body.value("name", "");
            std::string desc = body.value("description", "");
            std::string content = body.value("content", "");
            if (!valid_skill_name(name)) {
                send_error(resp, 400, "skill name must be kebab-case (a-z 0-9 -)"); return;
            }
            if (content.empty()) { send_error(resp, 400, "content required"); return; }
            fs::path dir = global_skills_root() / name;
            std::error_code ec;
            if (fs::exists(dir, ec)) { send_error(resp, 409, "skill already exists"); return; }
            fs::create_directories(dir, ec);
            std::ofstream f(dir / "SKILL.md", std::ios::trunc);
            if (!f) { send_error(resp, 500, "cannot write skill file"); return; }
            f << "---\nname: " << name << "\n"
              << "description: " << (desc.empty() ? name : desc) << "\n"
              << "version: 1.0.0\n---\n\n"
              << content << "\n";
            f.close();
            send_json(resp, json{{"created", name}}, 201);
        });

    // ── DELETE /api/skills/:name ──────────────────────────────────────────
    server.delete_("/api/skills/:name",
        [&agents](const OmniNet::HTTP::Request& req, OmniNet::HTTP::Response& resp) {
            auto seg = path_segments(req.path());   // api skills :name
            if (seg.size() < 3) { send_error(resp, 400, "missing name"); return; }
            if (!valid_skill_name(seg[2])) { send_error(resp, 400, "bad skill name"); return; }
            fs::path dir = global_skills_root() / seg[2];
            std::error_code ec;
            if (!fs::exists(dir, ec)) { send_error(resp, 404, "skill not found"); return; }
            // Remove every agent's symlink first so none dangle.
            for (auto& a : agents.list_agents()) {
                fs::path link = agent_skills_root(a.id) / seg[2];
                if (fs::is_symlink(link, ec)) fs::remove(link, ec);
            }
            fs::remove_all(dir, ec);
            send_json(resp, json{{"deleted", seg[2]}});
        });

    // ── POST /api/skills/install ─────────────────────────────────────────
    // {git_url, path?} — Phase 14.3 skill registry: shallow-clone a repo,
    // locate the skill dir (repo root or `path` subdir), validate SKILL.md,
    // copy into the global library. 409 if the name is taken.
    // The URL/path go through a strict charset allowlist AND single-quoting
    // before hitting the shell — injection-safe.
    server.post("/api/skills/install",
        [&agents](const OmniNet::HTTP::Request& req, OmniNet::HTTP::Response& resp) {
            json body;
            try { body = json::parse(req.bodyAsString()); }
            catch (...) { send_error(resp, 400, "invalid JSON"); return; }
            const std::string url = body.value("git_url", "");
            const std::string sub = body.value("path", "");
            auto safe = [](const std::string& v, size_t max) {
                if (v.empty() || v.size() > max) return false;
                for (unsigned char c : v) {
                    if (std::isalnum(c)) continue;
                    if (std::string(":/.-_@~%+").find((char)c) != std::string::npos) continue;
                    return false;
                }
                return true;
            };
            if (!safe(url, 300)) { send_error(resp, 400, "bad git_url"); return; }
            if (!sub.empty() && (!safe(sub, 120) || sub.find("..") != std::string::npos)) {
                send_error(resp, 400, "bad path"); return;
            }

            char tmpl[] = "/tmp/hive-skill-XXXXXX";
            if (!mkdtemp(tmpl)) { send_error(resp, 500, "tmp dir failed"); return; }
            const fs::path tmp = tmpl;
            struct Cleanup { fs::path p; ~Cleanup() {
                std::error_code ec; fs::remove_all(p, ec); fs::remove(p.string() + ".log", ec);
            } } cleanup{tmp};

            const std::string cmd = "timeout 90 git clone --depth 1 '" + url +
                                    "' '" + tmp.string() + "' >'" + tmp.string() + ".log' 2>&1";
            if (std::system(cmd.c_str()) != 0) {
                std::ifstream lf(tmp.string() + ".log");
                std::string tail, line;
                while (std::getline(lf, line)) { tail = line; }  // last line
                send_error(resp, 502, "git clone failed: " + tail);
                return;
            }

            const fs::path src = sub.empty() ? tmp : tmp / sub;
            std::error_code ec;
            if (!fs::exists(src / "SKILL.md", ec)) {
                send_error(resp, 400, "no SKILL.md at repo " +
                           (sub.empty() ? "root" : "path '" + sub + "'"));
                return;
            }
            json fm = frontmatter_of(src / "SKILL.md");
            std::string name = fm.value("name", "");
            if (name.empty()) name = src.filename().string();
            if (!valid_skill_name(name)) {
                send_error(resp, 400, "invalid skill name '" + name + "'");
                return;
            }
            fs::create_directories(global_skills_root(), ec);
            const fs::path dest = global_skills_root() / name;
            if (fs::exists(dest, ec)) {
                send_error(resp, 409, "skill '" + name + "' already installed");
                return;
            }
            fs::copy(src, dest, fs::copy_options::recursive, ec);
            if (ec) { send_error(resp, 500, "copy failed: " + ec.message()); return; }
            send_json(resp, json{{"installed", name},
                                 {"description", fm.value("description", "")},
                                 {"version", fm.value("version", "")}}, 201);
        });

    // ── GET /api/agents/:id/global-skills ─────────────────────────────────
    server.get("/api/agents/:id/global-skills",
        [&agents](const OmniNet::HTTP::Request& req, OmniNet::HTTP::Response& resp) {
            auto seg = path_segments(req.path());   // api agents :id global-skills
            if (seg.size() < 4) { send_error(resp, 400, "bad path"); return; }
            if (!agents.get_agent(seg[2])) { send_error(resp, 404, "agent not found"); return; }
            send_json(resp, json{{"skills", enabled_global_skills(seg[2])}});
        });

    // ── POST /api/agents/:id/global-skills ────────────────────────────────
    // {name, enabled} — create/remove the symlink. The link is RELATIVE
    // (../../../../skills/<name>) so it keeps resolving when the hivemind data
    // root is bind-mounted into an agent container at the same depth.
    server.post("/api/agents/:id/global-skills",
        [&agents](const OmniNet::HTTP::Request& req, OmniNet::HTTP::Response& resp) {
            auto seg = path_segments(req.path());
            if (seg.size() < 4) { send_error(resp, 400, "bad path"); return; }
            if (!agents.get_agent(seg[2])) { send_error(resp, 404, "agent not found"); return; }
            json body;
            try { body = json::parse(req.bodyAsString()); }
            catch (...) { send_error(resp, 400, "invalid JSON"); return; }
            std::string name = body.value("name", "");
            if (!valid_skill_name(name)) { send_error(resp, 400, "bad skill name"); return; }
            const bool enable = body.value("enabled", true);

            fs::path target = global_skills_root() / name;
            std::error_code ec;
            if (enable && !fs::exists(target / "SKILL.md", ec)) {
                send_error(resp, 404, "global skill not found"); return;
            }
            fs::path link = agent_skills_root(seg[2]) / name;
            if (enable) {
                // Don't clobber an agent-local skill with the same name.
                if (fs::exists(link, ec) && !is_global_link(link)) {
                    send_error(resp, 409,
                        "agent has a local skill named '" + name + "'"); return;
                }
                fs::create_directories(link.parent_path(), ec);
                fs::remove(link, ec);   // refresh existing link
                // ../../../.. from <base>/agents/<id>/.agents/skills/ (the dir
                // CONTAINING the link) → <base>
                fs::create_symlink(fs::path("../../../../skills") / name, link, ec);
                if (ec) { send_error(resp, 500, "symlink failed: " + ec.message()); return; }
            } else {
                if (fs::is_symlink(link, ec)) fs::remove(link, ec);
            }
            // Phase 33: worker agents get the skill FILES on their rig.
            if (auto ra = agents.get_agent(seg[2])) {
                if (enable) agent_tools::push_skill_to_worker(*ra, name);
                else        agent_tools::remove_skill_on_worker(*ra, name);
            }
            send_json(resp, json{{"name", name}, {"enabled", enable}});
        });
}
