// include/hivemind/routes/RosterRoutes.cpp
// -----------------------------------------------------------------------------
// Roster + chat REST API (HiveMind 2.0, Plan.md Phase 1).
// NOTE: OmniNet pathParam(i) is unreliable for multi-param routes — parse
// positional segments from req.path() instead (see project memory).
// -----------------------------------------------------------------------------

#include "hivemind/routes/RouteUtil.h"
#include "hivemind/routes/RosterRoutes.h"
#include "hivemind/AgentHost.h"
#include "hivemind/RoutineStore.h"
#include "hivemind/ThreadStore.h"

#include <nlohmann/json.hpp>

#include <string>
#include <vector>

using json = nlohmann::json;

namespace {
using hivemind::routes::path_segments;
using hivemind::routes::send_json;
using hivemind::routes::send_error;








json agent_json(const RosterAgent& a) {
    return {
        {"id", a.id}, {"name", a.name}, {"role", a.role},
        {"model", a.model}, {"workspace_mode", a.workspace_mode},
        {"container_id", a.container_id},
        {"require_approval", a.require_approval}, {"avatar", a.avatar},
        {"created_at", a.created_at}, {"updated_at", a.updated_at},
    };
}

json thread_json(const Thread& t) {
    return {
        {"id", t.id}, {"title", t.title}, {"kind", t.kind},
        {"created_at", t.created_at}, {"last_activity", t.last_activity},
        {"agent_ids", t.agent_ids},
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
                            ChatService& chat,
                            RoutineStore& routines) {

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
            bool req_appr = body.value("require_approval", false);
            try {
                auto a = agents.create_agent(name, role, model, ws, req_appr,
                                             body.value("avatar", ""));
                send_json(resp, agent_json(a), 201);
            } catch (const std::invalid_argument& e) {
                send_error(resp, 400, e.what());
            }
        });

    // ── GET /api/agents ───────────────────────────────────────────────────
    // busy: agent has a live turn in any thread (Phase 14.1 roster dots).
    server.get("/api/agents",
        [&agents, &chat](const OmniNet::HTTP::Request&, OmniNet::HTTP::Response& resp) {
            auto live = chat.agent_activity();
            json list = json::array();
            for (auto& a : agents.list_agents()) {
                auto j = agent_json(a);
                j["busy"] = live.count(a.id) > 0;
                list.push_back(std::move(j));
            }
            send_json(resp, json{{"agents", list}});
        });

    // ── GET /api/agents/activity ──────────────────────────────────────────
    // Per-agent live status across all threads (web UI Agents page).
    // Registered BEFORE /api/agents/:id so the literal path wins.
    server.get("/api/agents/activity",
        [&chat](const OmniNet::HTTP::Request&, OmniNet::HTTP::Response& resp) {
            json m = json::object();
            for (auto& [aid, t] : chat.agent_activity())
                m[aid] = json{{"busy", true},
                              {"cancelling", t.cancelling},
                              {"agent_name", t.agent_name},
                              {"partial_len", (int)t.partial.size()}};
            send_json(resp, json{{"agents", m}});
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
                    body.value("workspace_mode", existing->workspace_mode),
                    body.value("require_approval", existing->require_approval),
                    body.value("avatar", existing->avatar));
                // Name/model changes must not leak into a live host.
                registry.evict(a.id);
                // Phase 10 fix: renaming an agent must carry its daedalus
                // namespace (messages/memory) along — the dir is keyed by
                // the sanitized agent name.
                if (a.name != existing->name) {
                    try {
                        namespace fs = std::filesystem;
                        fs::path base = AgentStore::agents_base_dir() / a.id
                                      / ".daedalus";
                        fs::path oldns = base / ("daedalus-" + sanitize_agent_name(existing->name));
                        fs::path newns = base / ("daedalus-" + sanitize_agent_name(a.name));
                        std::error_code ec;
                        if (fs::exists(oldns, ec) && !fs::exists(newns, ec))
                            fs::rename(oldns, newns, ec);
                    } catch (...) { /* best effort — old dir remains */ }
                }
                send_json(resp, agent_json(a));
            } catch (const std::invalid_argument& e) {
                send_error(resp, 400, e.what());
            }
        });

    // Cascade delete: threads the agent was in are cleaned up (solo threads
    // deleted, group threads keep going without the agent), and its routines
    // are either deleted or moved to another agent via ?reassign_to=<id>.
    server.delete_("/api/agents/:id",
        [&agents, &threads, &registry, &routines](const OmniNet::HTTP::Request& req,
                             OmniNet::HTTP::Response& resp) {
            auto seg = path_segments(req.path());
            if (seg.size() < 3) { send_error(resp, 400, "missing id"); return; }
            const std::string id = seg[2];
            auto existing = agents.get_agent(id);
            if (!existing) { send_error(resp, 404, "agent not found"); return; }

            const std::string reassign_to = req.queryParameter("reassign_to", "");
            if (!reassign_to.empty() && reassign_to != id
                && !agents.get_agent(reassign_to)) {
                send_error(resp, 400, "reassign target agent not found"); return;
            }

            // 1. Routines: reassign (moving routine-thread participation to
            //    the new owner) or delete.
            int routines_reassigned = 0;
            if (!reassign_to.empty() && reassign_to != id) {
                std::vector<std::string> routine_threads;
                for (auto& r : routines.list())
                    if (r.agent_id == id) routine_threads.push_back(r.thread_id);
                routines_reassigned = routines.reassign_agent(id, reassign_to);
                for (auto& tid : routine_threads) {
                    threads.remove_participant(tid, id);
                    threads.add_participant(tid, reassign_to);
                }
            }
            const int routines_deleted = routines.remove_for_agent(id);

            // 2. Threads: solo threads die with the agent; group threads lose
            //    the participant (with a system note for the humans reading).
            int threads_deleted = 0, threads_left = 0;
            for (auto& t : threads.list_threads()) {
                auto& parts = t.agent_ids;
                if (std::find(parts.begin(), parts.end(), id) == parts.end())
                    continue;
                if (parts.size() <= 1) {
                    threads.delete_thread(t.id);
                    ++threads_deleted;
                } else {
                    threads.remove_participant(t.id, id);
                    threads.add_message(t.id, "system", "",
                        existing->name + " was removed from the hive.");
                    ++threads_left;
                }
            }

            // 3. The roster row itself + any live host process.
            agents.delete_agent(id);
            registry.evict(id);
            send_json(resp, json{
                {"deleted", true},
                {"threads_deleted", threads_deleted},
                {"threads_left", threads_left},
                {"routines_deleted", routines_deleted},
                {"routines_reassigned", routines_reassigned},
            });
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
            // DM dedup (Phase 11): reopen the existing 1:1 conversation —
            // but ONLY when the caller asks for it (roster click). An
            // explicit "New thread" must always create a fresh one.
            if (body.value("reuse_dm", false) && agent_ids.size() == 1
                && body.value("kind", "dm") != "group") {
                if (auto existing = threads.find_dm(agent_ids[0])) {
                    send_json(resp, thread_json(*existing), 200);
                    return;
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
        [&threads, &chat](const OmniNet::HTTP::Request& req, OmniNet::HTTP::Response& resp) {
            auto seg = path_segments(req.path());
            if (seg.size() < 3) { send_error(resp, 400, "missing id"); return; }
            chat.cancel_thread(seg[2]);   // stop live turns before deleting
            if (!threads.delete_thread(seg[2])) {
                send_error(resp, 404, "thread not found"); return;
            }
            send_json(resp, json{{"deleted", true}});
        });

    // ── POST /api/threads/:id/cancel — stop live turns (Phase 11.3) ──────
    server.post("/api/threads/:id/cancel",
        [&threads, &chat](const OmniNet::HTTP::Request& req,
                          OmniNet::HTTP::Response& resp) {
            auto seg = path_segments(req.path());   // api threads :id cancel
            if (seg.size() < 3) { send_error(resp, 400, "missing id"); return; }
            if (!threads.get_thread(seg[2])) {
                send_error(resp, 404, "thread not found"); return;
            }
            chat.cancel_thread(seg[2]);
            send_json(resp, json{{"cancelled", true}}, 202);
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
                    {"done", t.done}, {"cancelling", t.cancelling},
                });
            }
            send_json(resp, json{{"busy", chat.busy(seg[2])},
                                 {"turns", turns}});
        });
}
