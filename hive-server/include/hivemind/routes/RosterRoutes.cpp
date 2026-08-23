// include/hivemind/routes/RosterRoutes.cpp
// -----------------------------------------------------------------------------
// Roster + chat REST API (HiveMind 2.0, Plan.md Phase 1).
// NOTE: OmniNet pathParam(i) is unreliable for multi-param routes — parse
// positional segments from req.path() instead (see project memory).
// -----------------------------------------------------------------------------

#include "hivemind/routes/RosterRoutes.h"

#include <nlohmann/json.hpp>

#include <string>
#include <vector>

using json = nlohmann::json;

namespace {

std::vector<std::string> path_segments(std::string_view path) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : path) {
        if (c == '/') { if (!cur.empty()) { out.push_back(cur); cur.clear(); } }
        else cur += c;
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

void send_json(OmniNet::HTTP::Response& resp, const json& j, int status = 200) {
    resp = OmniNet::HTTP::Response::ok();
    resp.setStatus(static_cast<OmniNet::HTTP::StatusCode>(status));
    resp.setContentType("application/json");
    resp.setBody(j.dump());
}

void send_error(OmniNet::HTTP::Response& resp, int status, const std::string& msg) {
    send_json(resp, json{{"error", msg}}, status);
}

json agent_json(const RosterAgent& a) {
    return {
        {"id", a.id}, {"name", a.name}, {"role", a.role},
        {"model", a.model}, {"workspace_mode", a.workspace_mode},
        {"container_id", a.container_id},
        {"created_at", a.created_at}, {"updated_at", a.updated_at},
    };
}

json thread_json(const Thread& t) {
    return {
        {"id", t.id}, {"title", t.title}, {"kind", t.kind},
        {"created_at", t.created_at}, {"agent_ids", t.agent_ids},
    };
}

json message_json(const ThreadMessage& m) {
    return {
        {"id", m.id}, {"thread_id", m.thread_id},
        {"sender_kind", m.sender_kind}, {"sender_id", m.sender_id},
        {"body", m.body}, {"created_at", m.created_at},
    };
}

} // namespace

void register_roster_routes(OmniNet::HTTP::Server& server,
                            AgentStore& agents,
                            ThreadStore& threads,
                            AgentRegistry& registry,
                            ChatService& chat) {

    // ── POST /api/agents ──────────────────────────────────────────────────
    server.post("/api/agents",
        [&agents](const OmniNet::HTTP::Request& req, OmniNet::HTTP::Response& resp) {
            json body;
            try { body = json::parse(req.bodyAsString()); }
            catch (...) { send_error(resp, 400, "invalid JSON"); return; }

            std::string name = body.value("name", "");
            std::string role = body.value("role", "");
            std::string model = body.value("model", "");
            std::string ws = body.value("workspace_mode", "shared");
            try {
                auto a = agents.create_agent(name, role, model, ws);
                send_json(resp, agent_json(a), 201);
            } catch (const std::invalid_argument& e) {
                send_error(resp, 400, e.what());
            }
        });

    // ── GET /api/agents ───────────────────────────────────────────────────
    server.get("/api/agents",
        [&agents](const OmniNet::HTTP::Request&, OmniNet::HTTP::Response& resp) {
            json list = json::array();
            for (auto& a : agents.list_agents()) list.push_back(agent_json(a));
            send_json(resp, json{{"agents", list}});
        });

    // ── GET/PATCH/DELETE /api/agents/:id ──────────────────────────────────
    server.get("/api/agents/:id",
        [&agents](const OmniNet::HTTP::Request& req, OmniNet::HTTP::Response& resp) {
            auto seg = path_segments(req.path());          // api agents :id
            if (seg.size() < 3) { send_error(resp, 400, "missing id"); return; }
            auto a = agents.get_agent(seg[2]);
            if (!a) { send_error(resp, 404, "agent not found"); return; }
            send_json(resp, agent_json(*a));
        });

    server.patch("/api/agents/:id",
        [&agents, &registry](const OmniNet::HTTP::Request& req,
                             OmniNet::HTTP::Response& resp) {
            auto seg = path_segments(req.path());
            if (seg.size() < 3) { send_error(resp, 400, "missing id"); return; }
            auto existing = agents.get_agent(seg[2]);
            if (!existing) { send_error(resp, 404, "agent not found"); return; }

            json body;
            try { body = json::parse(req.bodyAsString()); }
            catch (...) { send_error(resp, 400, "invalid JSON"); return; }

            try {
                auto a = agents.update_agent(seg[2],
                    body.value("name", existing->name),
                    body.value("role", existing->role),
                    body.value("model", existing->model),
                    body.value("workspace_mode", existing->workspace_mode));
                // Name/model changes must not leak into a live host.
                registry.evict(a.id);
                send_json(resp, agent_json(a));
            } catch (const std::invalid_argument& e) {
                send_error(resp, 400, e.what());
            }
        });

    server.delete_("/api/agents/:id",
        [&agents, &registry](const OmniNet::HTTP::Request& req,
                             OmniNet::HTTP::Response& resp) {
            auto seg = path_segments(req.path());
            if (seg.size() < 3) { send_error(resp, 400, "missing id"); return; }
            if (!agents.delete_agent(seg[2])) {
                send_error(resp, 404, "agent not found"); return;
            }
            registry.evict(seg[2]);
            send_json(resp, json{{"deleted", true}});
        });

    // ── POST /api/threads ─────────────────────────────────────────────────
    server.post("/api/threads",
        [&agents, &threads](const OmniNet::HTTP::Request& req,
                            OmniNet::HTTP::Response& resp) {
            json body;
            try { body = json::parse(req.bodyAsString()); }
            catch (...) { send_error(resp, 400, "invalid JSON"); return; }

            std::vector<std::string> agent_ids;
            if (body.contains("agent_ids") && body["agent_ids"].is_array()) {
                for (auto& v : body["agent_ids"])
                    if (v.is_string()) agent_ids.push_back(v.get<std::string>());
            }
            if (agent_ids.empty()) {
                send_error(resp, 400, "agent_ids must be a non-empty array");
                return;
            }
            for (const auto& id : agent_ids) {
                if (!agents.get_agent(id)) {
                    send_error(resp, 400, "unknown agent id: " + id); return;
                }
            }
            auto t = threads.create_thread(body.value("title", ""),
                                           body.value("kind", "dm"), agent_ids);
            send_json(resp, thread_json(t), 201);
        });

    // ── GET /api/threads ──────────────────────────────────────────────────
    server.get("/api/threads",
        [&threads](const OmniNet::HTTP::Request&, OmniNet::HTTP::Response& resp) {
            json list = json::array();
            for (auto& t : threads.list_threads()) list.push_back(thread_json(t));
            send_json(resp, json{{"threads", list}});
        });

    // ── GET/DELETE /api/threads/:id ───────────────────────────────────────
    server.get("/api/threads/:id",
        [&threads](const OmniNet::HTTP::Request& req, OmniNet::HTTP::Response& resp) {
            auto seg = path_segments(req.path());          // api threads :id
            if (seg.size() < 3) { send_error(resp, 400, "missing id"); return; }
            auto t = threads.get_thread(seg[2]);
            if (!t) { send_error(resp, 404, "thread not found"); return; }
            send_json(resp, thread_json(*t));
        });

    server.delete_("/api/threads/:id",
        [&threads](const OmniNet::HTTP::Request& req, OmniNet::HTTP::Response& resp) {
            auto seg = path_segments(req.path());
            if (seg.size() < 3) { send_error(resp, 400, "missing id"); return; }
            if (!threads.delete_thread(seg[2])) {
                send_error(resp, 404, "thread not found"); return;
            }
            send_json(resp, json{{"deleted", true}});
        });

    // ── GET /api/threads/:id/messages?after=<msg_id> ──────────────────────
    server.get("/api/threads/:id/messages",
        [&threads](const OmniNet::HTTP::Request& req, OmniNet::HTTP::Response& resp) {
            auto seg = path_segments(req.path());   // api threads :id messages
            if (seg.size() < 4) { send_error(resp, 400, "missing id"); return; }
            if (!threads.get_thread(seg[2])) {
                send_error(resp, 404, "thread not found"); return;
            }
            std::string after = req.queryParameter("after", "");
            json list = json::array();
            for (auto& m : threads.list_messages(seg[2], after))
                list.push_back(message_json(m));
            send_json(resp, json{{"messages", list}});
        });

    // ── POST /api/threads/:id/messages ────────────────────────────────────
    server.post("/api/threads/:id/messages",
        [&threads, &chat](const OmniNet::HTTP::Request& req,
                          OmniNet::HTTP::Response& resp) {
            auto seg = path_segments(req.path());
            if (seg.size() < 4) { send_error(resp, 400, "missing id"); return; }
            if (!threads.get_thread(seg[2])) {
                send_error(resp, 404, "thread not found"); return;
            }
            json body;
            try { body = json::parse(req.bodyAsString()); }
            catch (...) { send_error(resp, 400, "invalid JSON"); return; }

            std::string text = body.value("body", "");
            if (text.empty()) { send_error(resp, 400, "body must not be empty"); return; }

            try {
                std::string msg_id = chat.post_user_message(seg[2], text);
                send_json(resp, json{{"message_id", msg_id},
                                     {"status", "processing"}}, 202);
            } catch (const std::invalid_argument& e) {
                send_error(resp, 400, e.what());
            }
        });

    // ── GET /api/threads/:id/activity ─────────────────────────────────────
    server.get("/api/threads/:id/activity",
        [&threads, &chat](const OmniNet::HTTP::Request& req,
                          OmniNet::HTTP::Response& resp) {
            auto seg = path_segments(req.path());   // api threads :id activity
            if (seg.size() < 4) { send_error(resp, 400, "missing id"); return; }
            if (!threads.get_thread(seg[2])) {
                send_error(resp, 404, "thread not found"); return;
            }
            json turns = json::array();
            for (auto& t : chat.activity(seg[2])) {
                turns.push_back({
                    {"agent_id", t.agent_id}, {"agent_name", t.agent_name},
                    {"partial", t.partial}, {"error", t.error},
                    {"done", t.done},
                });
            }
            send_json(resp, json{{"busy", chat.busy(seg[2])},
                                 {"turns", turns}});
        });
}
