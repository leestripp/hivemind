// include/hivemind/routes/RosterRoutes.cpp
// -----------------------------------------------------------------------------
// Roster + chat REST API (HiveMind 2.0, Plan.md Phase 1).
// NOTE: OmniNet pathParam(i) is unreliable for multi-param routes — parse
// positional segments from req.path() instead (see project memory).
// -----------------------------------------------------------------------------

#include "hivemind/routes/RouteUtil.h"
#include "hivemind/routes/RosterRoutes.h"
#include "hivemind/AgentHost.h"
#include "hivemind/ContainerManager.h"

#include <iostream>
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
        {"worker", a.worker},
        {"mcp_servers", json::parse(a.mcp_servers, nullptr, false)},  // Phase 33
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
    // attachments (Phase 18): pass through as parsed JSON; "[]" default.
    json atts = json::parse(m.attachments_json, nullptr, false);
    if (!atts.is_array()) atts = json::array();
    return {
        {"id", m.id}, {"thread_id", m.thread_id},
        {"sender_kind", m.sender_kind}, {"sender_id", m.sender_id},
        {"body", m.body}, {"created_at", m.created_at},
        {"attachments", atts},
    };
}

} // namespace

void register_roster_routes(OmniNet::HTTP::Server& server,
                            AgentStore& agents,
                            ThreadStore& threads,
                            AgentRegistry& registry,
                            ChatService& chat,
                            RoutineStore& routines) {

    // ── GET /api/templates — teammate templates (Phase 19) ─────────────
    // Static catalog: pre-filled personas for the first-run / new-agent
    // flow. Clients fetch + pre-fill the create form (or one-click add);
    // creation still goes through POST /api/agents unchanged.
    server.get("/api/templates",
        [](const OmniNet::HTTP::Request&, OmniNet::HTTP::Response& resp) {
            // One preset per department head (matches the client's
            // data/avatars/ set). DRIVE_HINT is appended to every role so
            // agents file their work in the right department folder.
            static const std::string DRIVE_HINT =
                " Read /workspace/index.md first — it maps the company "
                "drive and shows which department folder your work "
                "belongs in.";
            static const json templates = json::array({
                {{"id", "chief"}, {"name", "Chief"}, {"avatar", "executive"},
                 {"require_approval", false},
                 {"role", "Chief of staff — coordinates the whole team. Uses "
                         "list_agents and message_agent to delegate work to "
                         "the right department head, tracks open threads and "
                         "deliverables, resolves conflicts between "
                         "departments, and reports progress back to the user "
                         "concisely." + DRIVE_HINT}},
                {{"id", "eng"}, {"name", "Eng"}, {"avatar", "engineering-female"},
                 {"require_approval", false},
                 {"role", "Head of engineering — reads and writes code in the "
                         "shared workspace, runs builds and tests, reviews "
                         "diffs carefully, and explains tradeoffs briefly. "
                         "Verifies claims by running things, not guessing; "
                         "keeps design notes next to the code." + DRIVE_HINT}},
                {{"id", "marketing"}, {"name", "Mark"}, {"avatar", "marketing-female"},
                 {"require_approval", false},
                 {"role", "Head of marketing — plans campaigns, drafts "
                         "announcements and social copy, and keeps a content "
                         "calendar in the workspace. Punchy, on-brand tone; "
                         "backs claims with market research and links."
                         + DRIVE_HINT}},
                {{"id", "sales"}, {"name", "Sales"}, {"avatar", "sales"},
                 {"require_approval", false},
                 {"role", "Head of sales — researches leads, drafts outreach "
                         "sequences, and keeps CRM notes and pipeline "
                         "summaries in the workspace. Friendly, concise "
                         "tone; always cites sources and logs objections "
                         "heard." + DRIVE_HINT}},
                {{"id", "people"}, {"name", "People"}, {"avatar", "hr-female"},
                 {"require_approval", false},
                 {"role", "Head of people — writes job descriptions, "
                         "onboarding checklists and team policies, and keeps "
                         "the org chart and roster notes current. Warm, "
                         "clear, discreet with sensitive topics."
                         + DRIVE_HINT}},
                {{"id", "finance"}, {"name", "Fin"}, {"avatar", "finance"},
                 {"require_approval", true},
                 {"role", "Head of finance — builds budgets and forecasts as "
                         "spreadsheets in the workspace, reconciles figures "
                         "across documents, and flags anomalies with exact "
                         "numbers. Precise and conservative; shows working."
                         + DRIVE_HINT}},
                {{"id", "support"}, {"name", "Support"}, {"avatar", "it-support-female"},
                 {"require_approval", true},
                 {"role", "Head of IT support — triages technical issues, "
                         "reproduces problems before fixing them, writes "
                         "runbooks and how-tos, and escalates with exact "
                         "error output and steps tried." + DRIVE_HINT}},
                {{"id", "design"}, {"name", "Design"}, {"avatar", "design"},
                 {"require_approval", false},
                 {"role", "Head of design — produces wireframes, SVG assets "
                         "and style guides in the workspace, reviews UI for "
                         "consistency and accessibility, and presents "
                         "options with rationale rather than one take."
                         + DRIVE_HINT}},
                {{"id", "legal"}, {"name", "Legal"}, {"avatar", "legal"},
                 {"require_approval", true},
                 {"role", "Head of legal — reviews contracts and policies, "
                         "drafts plain-language summaries with risk flags, "
                         "and keeps a register of obligations in the "
                         "workspace. Cautious, precise, always cites the "
                         "clause or source relied on." + DRIVE_HINT}},
                {{"id", "ops"}, {"name", "Ops"}, {"avatar", "operations"},
                 {"require_approval", true},
                 {"role", "Head of operations — monitors systems, triages "
                         "alerts, and runs routine checks. Prefers "
                         "checklists, gathers evidence before reporting, "
                         "and escalates with exact error output."
                         + DRIVE_HINT}},
            });
            send_json(resp, json{{"templates", templates}});
        });

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
            if (ws == "host") ws = "shared";   // host mode removed (2026-08-25)
            bool req_appr = body.value("require_approval", false);
            // Phase 30: "worker" = "local" (default) or an enrolled hive-worker
            // name. Placement errors surface at exec time (worker offline), so
            // an unknown name is accepted here — same contract as the PATCH.
            const std::string worker = body.value("worker", "local");
            try {
                auto a = agents.create_agent(name, role, model, ws, req_appr,
                                             body.value("avatar", ""), worker);
                send_json(resp, agent_json(a), 201);
            } catch (const std::invalid_argument& e) {
                send_error(resp, 400, e.what());
            }
        });

    // ── GET /api/agents ───────────────────────────────────────────────────
    // busy: agent has a live turn in any thread (Phase 14.1 roster dots).
    server.get("/api/agents",
        [&agents, &chat, &registry](const OmniNet::HTTP::Request&, OmniNet::HTTP::Response& resp) {
            auto live = chat.agent_activity();
            json list = json::array();
            for (auto& a : agents.list_agents()) {
                auto j = agent_json(a);
                j["busy"] = live.count(a.id) > 0;
                j["state"] = registry.agent_state(a.id);
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
    // ── GET /api/agents/:id/mcp-tools ─────────────────────────────────────
    // Phase 33 debug: what daedalus actually loaded/registered for MCP.
    server.get("/api/agents/:id/mcp-tools",
        [&registry](const OmniNet::HTTP::Request& req,
                    OmniNet::HTTP::Response& resp) {
            auto seg = path_segments(req.path());
            if (seg.size() < 4) { send_error(resp, 400, "missing id"); return; }
            auto* host = registry.find(seg[2]);
            if (!host) { send_json(resp, json{{"brain", "not constructed"}}); return; }
            send_json(resp, host->mcp_debug());
        });

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
                    body.value("avatar", existing->avatar),
                    body.value("worker", ""),   // "" = keep current
                    // Phase 33: accept array or string; "" = keep current
                    body.contains("mcp_servers")
                        ? (body["mcp_servers"].is_array()
                               ? body["mcp_servers"].dump()
                               : body.value("mcp_servers", ""))
                        : "");
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

            // 3. The roster row itself + any live host process + the
            //    container (Phase 28: deleting an agent must not orphan
            //    its podman container — best-effort, the row is gone
            //    either way).
            agents.delete_agent(id);
            registry.evict(id);
            try { container_manager().remove(id); }
            catch (const std::exception& e) {
                std::cerr << "[roster] container teardown for " << id
                          << " failed: " << e.what() << "\n";
            }
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
            // Phase 18: optional attachments array (metadata from prior
            // POST /attachments calls — {name, path, host_path, size}).
            std::string atts = "[]";
            if (body.contains("attachments")) {
                if (!body["attachments"].is_array()) {
                    send_error(resp, 400, "attachments must be an array");
                    return;
                }
                atts = body["attachments"].dump();
            }
            if (text.empty() && atts == "[]") {
                send_error(resp, 400, "body must not be empty"); return;
            }

            try {
                std::string msg_id = chat.post_user_message(seg[2], text, atts);
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
