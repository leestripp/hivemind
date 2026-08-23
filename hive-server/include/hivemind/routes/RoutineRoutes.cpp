// include/hivemind/routes/RoutineRoutes.cpp
// -----------------------------------------------------------------------------
// Routines (scheduled agent prompts) + agent skills (HiveMind 2.0, Phase 5).
// -----------------------------------------------------------------------------

#include "hivemind/routes/RouteUtil.h"
#include "hivemind/routes/RoutineRoutes.h"
#include "hivemind/Schedule.h"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using json = nlohmann::json;
namespace fs = std::filesystem;

namespace {
using hivemind::routes::path_segments;
using hivemind::routes::send_json;
using hivemind::routes::send_error;








json routine_json(const Routine& r) {
    json sched;
    try { sched = json::parse(r.schedule_json); }
    catch (...) { sched = json::object(); }
    return {
        {"id", r.id}, {"agent_id", r.agent_id}, {"thread_id", r.thread_id},
        {"name", r.name}, {"prompt", r.prompt},
        {"schedule", sched},
        {"schedule_text", hm::describe_schedule(sched)},
        {"next_fire_at", r.next_fire_at}, {"enabled", r.enabled},
        {"created_at", r.created_at}, {"last_fired_at", r.last_fired_at},
    };
}

// Skills live at <agent_dir>/.agents/skills/<name>/SKILL.md (daedalus
// discovers the project-relative .agents/skills directory).
fs::path skills_root(const std::string& agent_id) {
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

} // namespace

void register_routine_routes(OmniNet::HTTP::Server& server,
                             RoutineStore& routines,
                             AgentStore& agents,
                             ThreadStore& threads,
                             ChatService& chat) {

    // ── POST /api/routines ────────────────────────────────────────────────
    server.post("/api/routines",
        [&routines, &agents, &threads](const OmniNet::HTTP::Request& req,
                                       OmniNet::HTTP::Response& resp) {
            json body;
            try { body = json::parse(req.bodyAsString()); }
            catch (...) { send_error(resp, 400, "invalid JSON"); return; }

            std::string agent_id = body.value("agent_id", "");
            std::string name = body.value("name", "");
            std::string prompt = body.value("prompt", "");
            json schedule = body.value("schedule", json::object());

            if (!agents.get_agent(agent_id)) { send_error(resp, 400, "unknown agent"); return; }
            if (name.empty() || prompt.empty()) { send_error(resp, 400, "name and prompt required"); return; }
            if (auto err = hm::validate_schedule(schedule); !err.empty()) {
                send_error(resp, 400, err); return;
            }

            // Each routine gets its own thread so fires don't spam DMs.
            auto t = threads.create_thread("routine: " + name, "dm", {agent_id});
            auto next = hm::compute_next_fire(schedule, hm::now_iso_local(),
                                              hm::now_iso_local());
            auto r = routines.create(agent_id, t.id, name, prompt,
                                     schedule.dump(), next.value_or(""));
            send_json(resp, routine_json(r), 201);
        });

    // ── GET /api/routines ─────────────────────────────────────────────────
    server.get("/api/routines",
        [&routines](const OmniNet::HTTP::Request&, OmniNet::HTTP::Response& resp) {
            json list = json::array();
            for (auto& r : routines.list()) list.push_back(routine_json(r));
            send_json(resp, json{{"routines", list}});
        });

    // ── POST /api/routines/:id/run ────────────────────────────────────────
    server.post("/api/routines/:id/run",
        [&routines, &chat](const OmniNet::HTTP::Request& req,
                           OmniNet::HTTP::Response& resp) {
            auto seg = path_segments(req.path());
            if (seg.size() < 3) { send_error(resp, 400, "missing id"); return; }
            auto r = routines.get(seg[2]);
            if (!r) { send_error(resp, 404, "routine not found"); return; }
            try {
                chat.post_user_message(r->thread_id, r->prompt);
                send_json(resp, json{{"fired", true}, {"thread_id", r->thread_id}}, 202);
            } catch (const std::exception& e) {
                send_error(resp, 500, e.what());
            }
        });

    // ── POST /api/routines/:id/enabled ────────────────────────────────────
    server.post("/api/routines/:id/enabled",
        [&routines](const OmniNet::HTTP::Request& req,
                    OmniNet::HTTP::Response& resp) {
            auto seg = path_segments(req.path());
            if (seg.size() < 3) { send_error(resp, 400, "missing id"); return; }
            json body;
            try { body = json::parse(req.bodyAsString()); }
            catch (...) { send_error(resp, 400, "invalid JSON"); return; }
            bool en = body.value("enabled", true);
            if (!routines.set_enabled(seg[2], en)) {
                send_error(resp, 404, "routine not found"); return;
            }
            send_json(resp, json{{"enabled", en}});
        });

    // ── DELETE /api/routines/:id ──────────────────────────────────────────
    server.delete_("/api/routines/:id",
        [&routines](const OmniNet::HTTP::Request& req,
                    OmniNet::HTTP::Response& resp) {
            auto seg = path_segments(req.path());
            if (seg.size() < 3) { send_error(resp, 400, "missing id"); return; }
            if (!routines.remove(seg[2])) { send_error(resp, 404, "routine not found"); return; }
            send_json(resp, json{{"deleted", true}});
        });

    // ── GET /api/agents/:id/skills ────────────────────────────────────────
    server.get("/api/agents/:id/skills",
        [&agents](const OmniNet::HTTP::Request& req, OmniNet::HTTP::Response& resp) {
            auto seg = path_segments(req.path());
            if (seg.size() < 4) { send_error(resp, 400, "bad path"); return; }
            if (!agents.get_agent(seg[2])) { send_error(resp, 404, "agent not found"); return; }
            json list = json::array();
            std::error_code ec;
            fs::path root = skills_root(seg[2]);
            if (fs::exists(root, ec))
                for (auto& e : fs::directory_iterator(root, ec))
                    if (e.is_directory() && fs::exists(e.path() / "SKILL.md"))
                        list.push_back(e.path().filename().string());
            send_json(resp, json{{"skills", list}});
        });

    // ── POST /api/agents/:id/skills ───────────────────────────────────────
    server.post("/api/agents/:id/skills",
        [&agents](const OmniNet::HTTP::Request& req, OmniNet::HTTP::Response& resp) {
            auto seg = path_segments(req.path());
            if (seg.size() < 4) { send_error(resp, 400, "bad path"); return; }
            if (!agents.get_agent(seg[2])) { send_error(resp, 404, "agent not found"); return; }
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

            fs::path dir = skills_root(seg[2]) / name;
            std::error_code ec;
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

    // ── DELETE /api/agents/:id/skills/:name ───────────────────────────────
    server.delete_("/api/agents/:id/skills/:name",
        [&agents](const OmniNet::HTTP::Request& req, OmniNet::HTTP::Response& resp) {
            auto seg = path_segments(req.path());  // api agents :id skills :name
            if (seg.size() < 5) { send_error(resp, 400, "bad path"); return; }
            if (!agents.get_agent(seg[2])) { send_error(resp, 404, "agent not found"); return; }
            if (!valid_skill_name(seg[4])) { send_error(resp, 400, "bad skill name"); return; }
            fs::path dir = skills_root(seg[2]) / seg[4];
            std::error_code ec;
            if (!fs::exists(dir, ec)) { send_error(resp, 404, "skill not found"); return; }
            fs::remove_all(dir, ec);
            send_json(resp, json{{"deleted", seg[4]}});
        });
}
