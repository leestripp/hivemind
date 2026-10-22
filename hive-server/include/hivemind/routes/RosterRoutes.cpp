// include/hivemind/routes/RosterRoutes.cpp
// -----------------------------------------------------------------------------
// Roster + chat REST API (HiveMind 2.0, Plan.md Phase 1).
// NOTE: OmniNet pathParam(i) is unreliable for multi-param routes — parse
// positional segments from req.path() instead (see project memory).
// -----------------------------------------------------------------------------

#include "hivemind/routes/RouteUtil.h"
#include "hivemind/routes/RosterRoutes.h"
#include "hivemind/MessageHeader.h"
#include "hivemind/ChatService.h"
#include "hivemind/DeliveryStore.h"
#include "hivemind/HubRouter.h"

#include <iostream>
#include "hivemind/RoutineStore.h"
#include "hivemind/ServerConfig.h"
#include "hivemind/ThreadStore.h"
#include "hivemind/WorkerStore.h"

#include <nlohmann/json.hpp>

#include <cctype>
#include <string>
#include <vector>

using json = nlohmann::json;

namespace {
using hivemind::routes::path_segments;
using hivemind::routes::send_json;
using hivemind::routes::send_error;
using hivemind::routes::normalize_thinking_level;








json agent_json(const RosterAgent& a) {
    return {
        {"id", a.id}, {"name", a.name}, {"role", a.role},
        {"model", a.model}, {"workspace_mode", a.workspace_mode},
        {"container_id", a.container_id},
        {"require_approval", a.require_approval}, {"avatar", a.avatar},
        {"worker", a.worker}, {"type", a.type},                   // Phase 35
        {"memory", a.memory}, {"cpus", a.cpus},                  // Phase 69
        {"thinking_level", a.thinking_level},                     // 2026-10-01
        {"host_agent", a.host_agent},                               // Phase 72
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
    // stats (Phase 64): parsed object; omitted when the turn recorded none.
    json stats = json::parse(m.stats_json, nullptr, false);
    json out = {
        {"id", m.id}, {"thread_id", m.thread_id},
        {"sender_kind", m.sender_kind}, {"sender_id", m.sender_id},
        {"body", m.body}, {"created_at", m.created_at},
        {"attachments", atts},
    };
    if (stats.is_object() && !stats.empty()) out["stats"] = stats;
    // §MsgStatus (2026-10-22): lifecycle marker, omitted when unset —
    // same shape the POST .../status endpoint accepts/returns.
    if (!m.status.empty()) {
        out["status"] = json{{"state", m.status},
                             {"by", m.status_by},
                             {"at", m.status_at},
                             {"note", m.status_note}};
    }
    return out;
}

/// §MsgStatus: one shared serializer for route responses (current
/// state after a set/clear, and embedded in 409 conflict bodies).
json status_json_of(const ThreadMessage& m) {
    if (m.status.empty()) return json(nullptr);
    return json{{"state", m.status},
                {"by", m.status_by},
                {"at", m.status_at},
                {"note", m.status_note}};
}

} // namespace

// v2: an agent MUST be placed on an ENROLLED hive-worker. There is no
// hub-local execution anymore -- "local" (or any never-enrolled name)
// creates a zombie that never spawns and never errors (user hit this
// via the client 2026-08-30). Enrolled-but-offline stays allowed:
// reconcile spawns it when the worker connects.
// NB: static free function, NOT a local lambda -- route handlers outlive
// this function's scope, so captures of locals would dangle.
static bool validate_worker(WorkerStore& workers, const std::string& w,
                            std::string& err) {
    if (workers.get(w)) return true;
    std::string names;
    for (auto& wi : workers.list()) {
        if (!names.empty()) names += ", ";
        names += wi.name;
    }
    err = "unknown worker '" + w + "' (enrolled workers: " +
          (names.empty() ? std::string("none -- enroll one first") : names) + ")";
    return false;
}

// Phase 35: agent type must be backed by the target worker's advertised
// caps (hello frame). "" / "default" always valid; "gpu" needs caps.gpu.
static bool validate_type(WorkerStore& workers, const std::string& w,
                          const std::string& type, std::string& err) {
    if (type.empty() || type == "default") return true;
    auto wi = workers.get(w);
    json caps;
    if (wi) { try { caps = json::parse(wi->caps); } catch (...) {} }
    if (caps.is_object() && caps.contains(type)) return true;
    err = "worker '" + w + "' has no '" + type + "' capability"
          " (its caps: " + (caps.is_object() ? caps.dump() : "{}") + ")";
    return false;
}

// Phase 72: host agents run BARE-METAL with passwordless sudo — no sandbox.
// Two-level opt-in: the per-agent roster flag (ack-gated below) AND the
// worker's own allow_host_agents config, advertised as the "host_agents"
// hello cap. A hub route can never conjure a host agent on a rig whose
// operator didn't opt in.
constexpr std::string_view kHostAgentAck = "I UNDERSTAND";
constexpr std::string_view kHostAgentWarning =
    "a host agent runs WITHOUT a sandbox, as a real user with sudo, and can "
    "install software, modify system files, and DESTROY the machine. Only "
    "use it on a machine you don't mind losing.";

static bool validate_host_capable(WorkerStore& workers,
                                  const std::string& w, std::string& err) {
    auto wi = workers.get(w);
    json caps;
    if (wi) { try { caps = json::parse(wi->caps); } catch (...) {} }
    if (caps.is_object() && caps.value("host_agents", false)) return true;
    err = "worker '" + w + "' has NOT opted in to host agents "
          "(set allow_host_agents in its config; its caps: " +
          (caps.is_object() ? caps.dump() : "{}") + ")";
    return false;
}

// Phase 69: memory must be a podman size string — digits with an optional
// b/k/m/g/t suffix (case-insensitive). "" and "default" pass through (they
// mean keep/clear in the update contract, worker default at create).
static bool validate_memory(const std::string& m) {
    if (m.empty() || m == "default") return true;
    size_t i = 0;
    while (i < m.size() && m[i] >= '0' && m[i] <= '9') ++i;
    if (i == 0) return false;
    if (i == m.size()) return true;   // plain bytes
    if (i + 1 != m.size()) return false;
    const char u = (char)tolower((unsigned char)m[i]);
    return u == 'b' || u == 'k' || u == 'm' || u == 'g' || u == 't';
}

void register_roster_routes(OmniNet::HTTP::Server& server,
                            AgentStore& agents,
                            ThreadStore& threads,
                            HubRouter& hub,
                            ChatService& chat,
                            RoutineStore& routines,
                            WorkerStore& workers) {

    // v2: an agent MUST be placed on an ENROLLED hive-worker. There is no
    // hub-local execution anymore -- "local" (or any never-enrolled name)
    // creates a zombie that never spawns and never errors (user hit this
    // via the client 2026-08-30). Enrolled-but-offline stays allowed:
    // reconcile spawns it when the worker connects.


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
                "belongs in. /workspace is shared with the agents on "
                "YOUR worker only — it is NOT synced between workers, "
                "so cross-rig hand-offs go through the thread, not the "
                "filesystem.";
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
        [&agents, &hub, &workers](const OmniNet::HTTP::Request& req, OmniNet::HTTP::Response& resp) {
            json body;
            try { body = json::parse(req.bodyAsString()); }
            catch (...) { send_error(resp, 400, "invalid JSON"); return; }

            std::string name = body.value("name", "");
            std::string role = body.value("role", "");
            std::string model = body.value("model", "");
            std::string ws = body.value("workspace_mode", "shared");
            if (ws == "host") ws = "shared";   // host mode removed (2026-08-25)
            bool req_appr = body.value("require_approval", false);
            // an unknown name is accepted here — same contract as the PATCH.
            const std::string worker = body.value("worker", "");
            const std::string type = body.value("type", "");   // Phase 35
            // Phase 69: optional container resource caps.
            const std::string memory = body.value("memory", "");
            double cpus = 0;
            if (body.contains("cpus")) {
                if (!body["cpus"].is_number() || body["cpus"].get<double>() < 0) {
                    send_error(resp, 400, "cpus must be a number >= 0"); return;
                }
                cpus = body["cpus"].get<double>();
            }
            if (!validate_memory(memory)) {
                send_error(resp, 400,
                    "memory must be a podman size like \"4g\" or \"1536m\"");
                return;
            }
            // 2026-10-01: thinking-level override; "" = inherit from model.
            std::string thinking_level = body.value("thinking_level", "");
            if (!normalize_thinking_level(thinking_level)) {
                send_error(resp, 400,
                    "thinking_level must be one of default|none|low|high|max");
                return;
            }
            // create has nothing to clear: default == inherit ("")
            if (thinking_level == "default") thinking_level.clear();
            {
                std::string werr;
                if (!validate_worker(workers, worker, werr)) {
                    send_error(resp, 400, werr); return;
                }
                if (!validate_type(workers, worker, type, werr)) {
                    send_error(resp, 400, werr); return;
                }
            }
            // Phase 72: host agent gate — ack + worker opt-in + forced
            // approvals. The ack string is an act of informed consent, so
            // a client can never flip this flag silently.
            const bool host_agent = body.value("host_agent", false);
            if (host_agent) {
                if (body.value("host_agent_ack", "") != kHostAgentAck) {
                    send_error(resp, 400,
                        std::string("host_agent requires host_agent_ack: \"") +
                        std::string(kHostAgentAck) + "\" — " +
                        std::string(kHostAgentWarning));
                    return;
                }
                std::string herr;
                if (!validate_host_capable(workers, worker, herr)) {
                    send_error(resp, 409, herr); return;
                }
                req_appr = true;   // shell commands park in the approval queue
                std::cerr << "HOST AGENT created: '" << name << "' on worker '"
                          << worker << "' — ack received ("
                          << kHostAgentAck << "); unsandboxed sudo\n";
            }
            try {
                auto a = agents.create_agent(name, role, model, ws, req_appr,
                                             body.value("avatar", ""), worker,
                                             type, memory, cpus,
                                             thinking_level, host_agent);
                // Phase C: spawn the brain on its worker right away
                // (idempotent; offline worker spawns on reconnect).
                hub.reconcile_worker(a.worker);
                send_json(resp, agent_json(a), 201);
            } catch (const std::invalid_argument& e) {
                send_error(resp, 400, e.what());
            }
        });

    // ── GET /api/agents ───────────────────────────────────────────────────
    // busy: agent has a live turn in any thread (Phase 14.1 roster dots).
    server.get("/api/agents",
        [&agents, &chat, &hub](const OmniNet::HTTP::Request&, OmniNet::HTTP::Response& resp) {
            auto live = chat.agent_activity();
            json list = json::array();
            for (auto& a : agents.list_agents()) {
                auto j = agent_json(a);
                j["busy"] = live.count(a.id) > 0;
                j["state"] = hub.agent_state(a.id);
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
        [&agents, &hub, &workers](const OmniNet::HTTP::Request& req,
                             OmniNet::HTTP::Response& resp) {
            auto seg = path_segments(req.path());
            if (seg.size() < 3) { send_error(resp, 400, "missing id"); return; }
            auto existing = agents.get_agent(seg[2]);
            if (!existing) { send_error(resp, 404, "agent not found"); return; }

            json body;
            try { body = json::parse(req.bodyAsString()); }
            catch (...) { send_error(resp, 400, "invalid JSON"); return; }

            const std::string ptype = body.value("type", "");  // Phase 35
            // Phase 69: "" = keep, "default" = clear to worker default.
            const std::string pmemory = body.value("memory", "");
            if (!validate_memory(pmemory)) {
                send_error(resp, 400,
                    "memory must be a podman size like \"4g\" or \"1536m\"");
                return;
            }
            double pcpus = -1;   // -1 = keep current
            if (body.contains("cpus")) {
                if (!body["cpus"].is_number() || body["cpus"].get<double>() < 0) {
                    send_error(resp, 400, "cpus must be a number >= 0"); return;
                }
                pcpus = body["cpus"].get<double>();
            }
            // 2026-10-01: "" = keep, "default" = clear, else set.
            std::string pthinking = body.value("thinking_level", "");
            if (!normalize_thinking_level(pthinking)) {
                send_error(resp, 400,
                    "thinking_level must be one of default|none|low|high|max");
                return;
            }
            const std::string pw = body.value("worker", "");
            {
                if (!pw.empty()) {
                    std::string werr;
                    if (!validate_worker(workers, pw, werr)) {
                        send_error(resp, 400, werr); return;
                    }
                }
                // validate the EFFECTIVE type against the worker it would
                // run on — covers both a type change AND moving an existing
                // gpu agent onto a gpu-less worker (ptype empty, pw set).
                const std::string eff_type = ptype == "default" ? ""
                    : ptype.empty() ? existing->type : ptype;
                if (!eff_type.empty()) {
                    std::string werr;
                    const std::string eff = pw.empty() ? existing->worker : pw;
                    if (!validate_type(workers, eff, eff_type, werr)) {
                        send_error(resp, 400, werr); return;
                    }
                }
            }
            // Phase 72: host_agent tri-state — absent = keep, false = clear
            // (safe direction, no ack), true = enable (ack + caps).
            int phost = -1;
            if (body.contains("host_agent")) {
                if (!body["host_agent"].is_boolean()) {
                    send_error(resp, 400, "host_agent must be a boolean");
                    return;
                }
                phost = body["host_agent"].get<bool>() ? 1 : 0;
            }
            const bool eff_host = phost == 1 ||
                                  (phost < 0 && existing->host_agent);
            if (phost == 1 &&
                body.value("host_agent_ack", "") != kHostAgentAck) {
                send_error(resp, 400,
                    std::string("enabling host_agent requires host_agent_ack: "
                                "\"") + std::string(kHostAgentAck) + "\" — " +
                    std::string(kHostAgentWarning));
                return;
            }
            if (eff_host) {
                // Caps must hold on the EFFECTIVE worker — covers enabling
                // AND moving an existing host agent to a new rig.
                std::string herr;
                const std::string effw = pw.empty() ? existing->worker : pw;
                if (!validate_host_capable(workers, effw, herr)) {
                    send_error(resp, 409, herr); return;
                }
            }
            bool preq_appr = body.value("require_approval",
                                        existing->require_approval);
            if (eff_host && !preq_appr) {
                if (body.contains("require_approval")) {
                    // Explicitly disabling approvals on a host agent is the
                    // other dangerous direction — same ack gate.
                    if (body.value("host_agent_ack", "") != kHostAgentAck) {
                        send_error(resp, 400,
                            std::string("disabling require_approval on a host "
                                        "agent requires host_agent_ack: \"") +
                            std::string(kHostAgentAck) + "\"");
                        return;
                    }
                    std::cerr << "HOST AGENT approvals disabled: '"
                              << existing->name << "' — ack received\n";
                } else if (phost == 1 && !existing->host_agent) {
                    // Freshly ENABLING host mode with no explicit approvals
                    // choice: the gate is forced ON. A routine update of an
                    // existing host agent (role/memory/mcp/...) keeps the
                    // operator's current choice — forcing it ON here silently
                    // re-gated Bob on an unrelated PATCH (2026-10-22).
                    preq_appr = true;
                }
            }
            if (phost == 1 && !existing->host_agent)
                std::cerr << "HOST AGENT enabled: '" << existing->name
                          << "' — ack received (" << kHostAgentAck
                          << "); unsandboxed sudo\n";
            try {
                auto a = agents.update_agent(seg[2],
                    body.value("name", existing->name),
                    body.value("role", existing->role),
                    body.value("model", existing->model),
                    body.value("workspace_mode", existing->workspace_mode),
                    preq_appr,
                    body.value("avatar", existing->avatar),
                    body.value("worker", ""),   // "" = keep current
                    // Phase 33: accept array or string; "" = keep current
                    body.contains("mcp_servers")
                        ? (body["mcp_servers"].is_array()
                               ? body["mcp_servers"].dump()
                               : body.value("mcp_servers", ""))
                        : "",
                    ptype,   // "" = keep, "default" = clear
                    pmemory, pcpus,    // Phase 69
                    pthinking,         // 2026-10-01
                    phost);            // Phase 72
                // Name/model changes must not leak into a live brain:
                // stop, then respawn with the new config (worker dedups).
                hub.stop_agent(a);
                hub.reconcile_worker(a.worker);
                // v2: the brain namespace lives on the worker's volume,
                // keyed by name; a rename starts a fresh brain on the next
                // spawn (old history stays orphaned on the volume).
                send_json(resp, agent_json(a));
            } catch (const std::invalid_argument& e) {
                send_error(resp, 400, e.what());
            }
        });

    // Cascade delete: threads the agent was in are cleaned up (solo threads
    // deleted, group threads keep going without the agent), and its routines
    // are either deleted or moved to another agent via ?reassign_to=<id>.
        // v2: explicit brain lifecycle on the owning worker (agent_stop is
    // graceful; start = spawn on the owning worker; offline -> 503).
    server.post("/api/agents/:id/start",
        [&agents, &hub](const OmniNet::HTTP::Request& req,
                        OmniNet::HTTP::Response& resp) {
            auto seg = path_segments(req.path());
            if (seg.size() < 4) { send_error(resp, 400, "bad path"); return; }
            auto a = agents.get_agent(seg[2]);
            if (!a) { send_error(resp, 404, "agent not found"); return; }
            try {
                hub.spawn_agent(*a);
                send_json(resp, json{{"started", true}});
            } catch (const std::exception& e) {
                send_error(resp, 503, e.what());
            }
        });

    server.post("/api/agents/:id/stop",
        [&agents, &hub](const OmniNet::HTTP::Request& req,
                        OmniNet::HTTP::Response& resp) {
            auto seg = path_segments(req.path());
            if (seg.size() < 4) { send_error(resp, 400, "bad path"); return; }
            auto a = agents.get_agent(seg[2]);
            if (!a) { send_error(resp, 404, "agent not found"); return; }
            hub.stop_agent(*a);
            send_json(resp, json{{"stopped", true}});
        });

    server.delete_("/api/agents/:id",
        [&agents, &threads, &hub, &routines](const OmniNet::HTTP::Request& req,
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
            // 3b. Tell the owning worker to tear the container down
            //     (v2: the worker owns podman; the row delete is
            //     authoritative either way - removal is best-effort).
            if (existing) {
                try { hub.remove_agent(*existing); }
                catch (const std::exception& e) {
                    std::cerr << "[roster] container teardown for " << id
                              << " failed: " << e.what() << "\n";
                }
                // Phase 72: deleting a host agent stops the process but
                // deliberately does NOT remove the unix user — deprovision
                // is manual/local/root-only. Leave a loud audit line so a
                // forgotten sudo user is never silent.
                if (existing->host_agent) {
                    // Mirror of the worker's host_user() mapping (lowercase,
                    // non-alnum -> '-').
                    std::string uname;
                    for (char ch : existing->name)
                        uname += (isalnum((unsigned char)ch) || ch == '-')
                                     ? (char)tolower((unsigned char)ch) : '-';
                    std::cerr << "HOST AGENT deleted: '" << existing->name
                              << "' — worker '" << existing->worker
                              << "' asked to deprovision unix user hivex-"
                              << uname << " (home archived; workspace never "
                              << "touched). If the worker was offline, run "
                              << "'sudo hive-worker deprovision-host-agent "
                              << existing->name << "' on that rig\n";
                }
            }
            agents.delete_agent(id);
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

    // ── GET /api/deliveries?agent=&state=&limit= (Phase 66e) ─────────
    // Admin observability over the durable delivery queue — the "one
    // query" diagnosis surface the 2026-09-26 silent drop lacked.
    server.get("/api/deliveries",
        [&chat](const OmniNet::HTTP::Request& req,
                OmniNet::HTTP::Response& resp) {
            const std::string agent = req.queryParameter("agent", "");
            const std::string state = req.queryParameter("state", "");
            int limit = 50;
            if (std::string ls = req.queryParameter("limit", "");
                !ls.empty()) {
                try { limit = std::max(1, std::min(500, std::stoi(ls))); }
                catch (...) {}
            }
            json arr = json::array();
            for (const auto& d : chat.deliveries_list(agent, state, limit))
                arr.push_back({
                    {"id", d.id},
                    {"kind", d.kind},
                    {"thread_id", d.thread_id},
                    {"target_agent_id", d.target_agent_id},
                    {"sender_agent_id", d.sender_agent_id},
                    {"sender_label", d.sender_label},
                    {"generation", d.generation},
                    {"state", d.state},
                    {"task_id", d.task_id},
                    {"source_message_id", d.source_message_id},
                    {"error", d.error},
                    {"read", chat.delivery_is_read(d)},
                    {"created_at", d.created_at},
                    {"dispatched_at", d.dispatched_at},
                    {"completed_at", d.completed_at}});
            send_json(resp, json{{"deliveries", arr}});
        });

    // ── GET /api/threads ──────────────────────────────────────────────────
    server.get("/api/threads",
        [&threads, &chat](const OmniNet::HTTP::Request&, OmniNet::HTTP::Response& resp) {
            json list = json::array();
            // Phase 67: ONE membership map for the whole list — never
            // per-row queries in a list route (66c N+1 / fd lesson).
            auto gmap = threads.thread_group_map();
            // 2026-10-01: unread badges ONLY for user-relevant threads.
            // Agent-to-agent threads (agents read those themselves — Lee:
            // "not my problem, it's the Agents") never badge the user. One
            // DISTINCT query for the whole list, never per-row.
            const auto user_threads = threads.user_posted_thread_ids();
            for (auto& t : threads.list_threads()) {
                auto j = thread_json(t);
                // inbox + DMs are always user-relevant; a group only once
                // the user has posted in it.
                // kind "routine": the SCHEDULER posts the fire prompt as
                // sender_kind 'user', so user_posted_thread_ids() counts
                // every routine thread — the gate would badge all of them.
                // Routine threads are agent workspace; results addressed to
                // the user are routed to the user INBOX thread (kind
                // "user", always badged). Never badge the routine itself.
                const bool user_relevant =
                    t.kind != "routine" &&
                    (t.kind == "user" || t.kind == "dm" ||
                     user_threads.count(t.id) > 0);
                j["unread"] = user_relevant ? chat.unread_for_user(t.id) : 0;
                auto it = gmap.find(t.id);
                j["group_id"] = (it != gmap.end()) ? json(it->second)
                                                   : json(nullptr);
                list.push_back(std::move(j));
            }
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

    // Phase 56: POST /api/threads/:id/clear — wipe history, keep thread.
    server.post("/api/threads/:id/clear",
        [&threads](const OmniNet::HTTP::Request& req,
                   OmniNet::HTTP::Response& resp) {
            auto seg = path_segments(req.path());
            if (seg.size() < 4) { send_error(resp, 400, "missing id"); return; }
            if (!threads.get_thread(seg[2])) {
                send_error(resp, 404, "thread not found"); return;
            }
            const int removed = threads.clear_messages(seg[2]);
            send_json(resp, json{{"cleared", true}, {"removed", removed}});
        });

    server.delete_("/api/threads/:id",
        [&threads, &chat, &routines](const OmniNet::HTTP::Request& req, OmniNet::HTTP::Response& resp) {
            auto seg = path_segments(req.path());
            if (seg.size() < 3) { send_error(resp, 400, "missing id"); return; }
            // Phase 44: the User Inbox is the one thread that must always
            // exist — agents need a guaranteed channel to the human.
            if (ThreadStore::is_user_thread(seg[2])) {
                send_error(resp, 400, "cannot delete the user inbox");
                return;
            }
            // A routine owns its thread: deleting it orphaned the routine
            // (manual run 500'd, scheduler then disabled it — 2026-09-16).
            // Block with a pointer at the owning routine instead.
            for (const auto& r : routines.list()) {
                if (r.thread_id == seg[2]) {
                    send_error(resp, 409,
                        "thread belongs to scheduled task '" + r.name +
                        "' — delete the task instead (it removes the thread)");
                    return;
                }
            }
            chat.cancel_thread(seg[2]);   // stop live turns before deleting
            if (!threads.delete_thread(seg[2])) {
                send_error(resp, 404, "thread not found"); return;
            }
            send_json(resp, json{{"deleted", true}});
        });

    // ── POST /api/agents/:id/cancel — stop the agent's turn(s) (Phase 57) ─
    server.post("/api/agents/:id/cancel",
        [&agents, &chat](const OmniNet::HTTP::Request& req,
                         OmniNet::HTTP::Response& resp) {
            auto seg = path_segments(req.path());
            if (seg.size() < 3 || !agents.get_agent(seg[2])) {
                send_error(resp, 404, "agent not found"); return;
            }
            const int n = chat.cancel_agent_turns(seg[2]);
            send_json(resp, json{{"cancelled", n}});
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

    // ── POST /api/threads/:id/read — the USER viewed the thread (66c) ───
    // Advances ONLY the reserved "user" read mark. Agent-addressed
    // delivery receipts key off each target agent's own mark, so the
    // user watching an agent-to-agent thread can never flip a
    // delivery to read; messages addressed to the user (inbox) are
    // exactly the ones this mark governs. Idempotent + monotonic.
    server.post("/api/threads/:id/read",
        [&threads, &chat](const OmniNet::HTTP::Request& req,
                          OmniNet::HTTP::Response& resp) {
            auto seg = path_segments(req.path());   // api threads :id read
            if (seg.size() < 3) { send_error(resp, 400, "missing id"); return; }
            if (!threads.get_thread(seg[2])) {
                send_error(resp, 404, "thread not found"); return;
            }
            chat.user_mark_read(seg[2]);
            send_json(resp, json{{"read", true}}, 202);
        });

    // ── Phase 67: user thread groups (folders — a thread lives in at
    // most one custom group; no membership row = its smart group) ────
    server.get("/api/thread-groups",
        [&threads](const OmniNet::HTTP::Request&, OmniNet::HTTP::Response& resp) {
            json list = json::array();
            for (auto& g : threads.list_groups()) {
                list.push_back(json{{"id", g.id},
                                    {"name", g.name},
                                    {"position", g.position},
                                    {"member_count", g.member_count}});
            }
            send_json(resp, json{{"groups", list}});
        });

    server.post("/api/thread-groups",
        [&threads](const OmniNet::HTTP::Request& req, OmniNet::HTTP::Response& resp) {
            json body = json::parse(req.body(), nullptr, /*exceptions=*/false);
            if (body.is_discarded() || !body.value("name", "").size()) {
                send_error(resp, 400, "name required"); return;
            }
            try {
                const std::string id = threads.create_group(body["name"]);
                send_json(resp, json{{"id", id}}, 201);
            } catch (const std::invalid_argument& e) {
                send_error(resp, 400, e.what());
            }
        });

    // PATCH /api/thread-groups/:id — rename. (position is assigned at
    // creation; manual reorder lands with drag-reorder later.)
    server.patch("/api/thread-groups/:id",
        [&threads](const OmniNet::HTTP::Request& req, OmniNet::HTTP::Response& resp) {
            auto seg = path_segments(req.path());   // api thread-groups :id
            if (seg.size() < 3) { send_error(resp, 400, "missing id"); return; }
            json body = json::parse(req.body(), nullptr, /*exceptions=*/false);
            if (body.is_discarded() || !body.value("name", "").size()) {
                send_error(resp, 400, "name required"); return;
            }
            try {
                threads.rename_group(seg[2], body["name"]);
                send_json(resp, json{{"renamed", true}});
            } catch (const std::invalid_argument& e) {
                send_error(resp, 404, e.what());
            }
        });

    // DELETE /api/thread-groups/:id — members revert to smart groups.
    server.delete_("/api/thread-groups/:id",
        [&threads](const OmniNet::HTTP::Request& req, OmniNet::HTTP::Response& resp) {
            auto seg = path_segments(req.path());   // api thread-groups :id
            if (seg.size() < 3) { send_error(resp, 400, "missing id"); return; }
            try {
                threads.delete_group(seg[2]);
                send_json(resp, json{{"deleted", true}}, 202);
            } catch (const std::invalid_argument& e) {
                send_error(resp, 404, e.what());
            }
        });

    // PUT /api/threads/:id/group — move a thread. {"group_id":"…"} into
    // a custom group; {"group_id":null} (or absent) back to its smart
    // group. Folder rule: re-moving replaces, never duplicates.
    server.put("/api/threads/:id/group",
        [&threads](const OmniNet::HTTP::Request& req, OmniNet::HTTP::Response& resp) {
            auto seg = path_segments(req.path());   // api threads :id group
            if (seg.size() < 3) { send_error(resp, 400, "missing id"); return; }
            json body = json::parse(req.body(), nullptr, /*exceptions=*/false);
            const std::string gid =
                (!body.is_discarded() && body["group_id"].is_string())
                    ? body["group_id"].get<std::string>() : "";
            try {
                threads.set_thread_group(seg[2], gid);
                send_json(resp, json{{"moved", true},
                                     {"group_id", gid.empty() ? json(nullptr)
                                                              : json(gid)}});
            } catch (const std::invalid_argument& e) {
                send_error(resp, 404, e.what());
            }
        });

    // ── §MsgStatus: POST /api/threads/:id/messages/:mid/status ───────────
    // Body: {"status":"claimed"|"done"|"verified", "by":"<agent name|user>",
    //        "note":"..."}  (note optional, <= 500 BYTES, must be valid
    // UTF-8 — we never truncate, we reject; code-craft §std::string IS
    // BYTES). Transition rules (QueenBee 2026-10-22 semantics):
    //   claimed  — first-claim-wins: 409 when someone ELSE holds any
    //              status; "user" may override (re-stamps by/at).
    //   done     — from empty or claimed (re-done re-stamps by/at/note).
    //   verified — only from done, and the verifier must be INDEPENDENT:
    //              by != the done-setter, unless by == "user" (403
    //              otherwise). QueenBee's anti-false-claim rule: the
    //              doer may never verify their own claim.
    // Trust note: `by` is caller-asserted (fleet key holders don't
    // forge); cryptographic caller identity is a later hardening.
    auto handle_set_status =
        [&threads](const OmniNet::HTTP::Request& req,
                   OmniNet::HTTP::Response& resp) {
            auto seg = path_segments(req.path());
            // api threads :id messages :mid status
            if (seg.size() < 6) { send_error(resp, 400, "missing id"); return; }
            json body = json::parse(req.body(), nullptr, /*exceptions=*/false);
            if (body.is_discarded() || !body.is_object()) {
                send_error(resp, 400, "invalid JSON"); return;
            }
            if (!body.contains("status") || !body["status"].is_string()) {
                send_error(resp, 400, "status must be a string"); return;
            }
            const std::string want = body["status"].get<std::string>();
            if (want != "claimed" && want != "done" && want != "verified") {
                send_error(resp, 400,
                    "status must be claimed, done or verified"); return;
            }
            std::string by;
            if (body.contains("by")) {
                if (!body["by"].is_string()) {
                    send_error(resp, 400, "by must be a string"); return;
                }
                by = body["by"].get<std::string>();
            }
            if (by.empty()) { send_error(resp, 400, "by must not be empty"); return; }
            std::string note;
            if (body.contains("note")) {
                if (!body["note"].is_string()) {
                    send_error(resp, 400, "note must be a string"); return;
                }
                note = body["note"].get<std::string>();
            }
            if (note.size() > 500) {
                send_error(resp, 400, "note too long (500 bytes max)"); return;
            }

            auto cur = threads.get_message(seg[4]);
            if (!cur || cur->thread_id != seg[2]) {
                send_error(resp, 404, "message not found"); return;
            }
            const bool is_user = by == "user";
            if (want == "claimed") {
                if (!cur->status.empty() && cur->status_by != by && !is_user) {
                    send_json(resp,
                        json{{"error", "already claimed"},
                             {"status", status_json_of(*cur)}}, 409);
                    return;
                }
            } else if (want == "verified") {
                if (cur->status != "done") {
                    send_json(resp,
                        json{{"error", "verify requires a done message"},
                             {"status", status_json_of(*cur)}}, 409);
                    return;
                }
                if (cur->status_by == by && !is_user) {
                    send_error(resp, 403,
                        "verifier must be independent of the doer"); return;
                }
            }
            try {
                threads.set_message_status(seg[2], seg[4], want, by, note);
            } catch (const std::invalid_argument& e) {
                send_error(resp, 404, e.what()); return;
            }
            auto updated = threads.get_message(seg[4]);
            send_json(resp, json{{"ok", true},
                                 {"message_id", seg[4]},
                                 {"status", status_json_of(*updated)}});
        };
    server.post("/api/threads/:id/messages/:mid/status", handle_set_status);

    // ── §MsgStatus: DELETE .../status — clear the marker ─────────────────
    server.delete_("/api/threads/:id/messages/:mid/status",
        [&threads](const OmniNet::HTTP::Request& req,
                   OmniNet::HTTP::Response& resp) {
            auto seg = path_segments(req.path());
            if (seg.size() < 6) { send_error(resp, 400, "missing id"); return; }
            try {
                threads.set_message_status(seg[2], seg[4], "", "", "");
            } catch (const std::invalid_argument& e) {
                send_error(resp, 404, e.what()); return;
            }
            send_json(resp, json{{"ok", true}, {"message_id", seg[4]}});
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
            // ?after=<id> — incremental live-poll path: everything newer,
            // unbounded by design (the client drains each 1.5s tick).
            if (!after.empty()) {
                json list = json::array();
                for (auto& m : threads.list_messages(seg[2], after))
                    list.push_back(message_json(m));
                send_json(resp, json{{"messages", list}});
                return;
            }
            // Phase 55: initial load / page-back — bounded window.
            // No cursor → the LATEST window; ?before=<id> pages older.
            std::string before = req.queryParameter("before", "");
            int limit = 0;
            if (std::string ls = req.queryParameter("limit", ""); !ls.empty()) {
                try { limit = std::stoi(ls); } catch (...) { limit = 0; }
            }
            if (limit <= 0 || limit > 5000)
                limit = ServerConfig::instance().message_window();
            bool has_more = false;
            json list = json::array();
            for (auto& m : threads.list_messages_window(seg[2], before,
                                                        limit, has_more))
                list.push_back(message_json(m));
            send_json(resp, json{{"messages", list},
                                 {"has_more", has_more}});
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

            // 2026-10-04 sweep fix: `body.value("body", "")` throws an
            // UNCAUGHT nlohmann type_error when the client posts a
            // non-string body (same latent class as the poison-wake
            // 500s) — the handler's try/catch only covers
            // post_user_message. Reject with a clean 400 instead.
            // Sweep fix: `body.value("body", "")` throws an UNCAUGHT
            // nlohmann type_error when the client posts a non-string
            // body (same latent class as the poison-wake 500s) — the
            // handler's try/catch only covers post_user_message.
            // Reject with a clean 400 regardless of attachments.
            if (body.contains("body") && !body["body"].is_string()) {
                send_error(resp, 400, "body must be a string"); return;
            }
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

            // 2026-10-04 (Plan.md §3.4): optional structured source.
            // Clients may only claim USER-channel sources ("user" or
            // "voice"→"voice-app"); routine:/agent:/phone:/system are
            // hub-internal — a client forging them would corrupt the
            // audit trail the header exists for.
            std::string source{hivemind::msg_source::kUser};
            if (body.contains("source")) {
                // Sweep fix: non-string source (a number, array, …)
                // must 400, not escape as an uncaught type_error.
                if (!body["source"].is_string()) {
                    send_error(resp, 400,
                               "source must be 'user' or 'voice'");
                    return;
                }
                std::string v = body.value("source", "");
                if (v == "voice") v = std::string(hivemind::msg_source::kVoiceApp);
                if (v != hivemind::msg_source::kUser &&
                    v != hivemind::msg_source::kVoiceApp) {
                    send_error(resp, 400,
                               "source must be 'user' or 'voice'");
                    return;
                }
                source = v;
            }

            try {
                std::string msg_id = chat.post_user_message(seg[2], text, atts,
                                                            source);
                send_json(resp, json{{"message_id", msg_id},
                                     {"status", "processing"}}, 202);
            } catch (const std::invalid_argument& e) {
                send_error(resp, 400, e.what());
            }
        });

    // ── POST /api/notifications (phone → agent DM, 2026-10-01) ─────
    // The Android app relays phone notifications (SMS, missed calls)
    // here; the hub appends each to the chosen agent's DM as a SILENT
    // system message — no turn triggered, no unread badge — so the
    // agent reads it from thread context when the user asks ("what
    // did I get?"). Same plumbing as relay/system messages.
    server.post("/api/notifications",
        [&threads, &agents](const OmniNet::HTTP::Request& req,
                            OmniNet::HTTP::Response& resp) {
            json body;
            try { body = json::parse(req.bodyAsString()); }
            catch (...) { send_error(resp, 400, "invalid JSON"); return; }
            const std::string agent_id = body.value("agent_id", "");
            if (agent_id.empty() || !agents.get_agent(agent_id)) {
                send_error(resp, 400, "unknown agent id"); return;
            }
            std::optional<Thread> dm = threads.find_dm(agent_id);
            if (!dm) dm = threads.create_thread("", "dm", {agent_id});
            const std::string app = body.value("app", "phone");
            const std::string title = body.value("title", "");
            const std::string text = body.value("text", "");
            std::string pretty = title.empty() ? text
                               : text.empty() ? title
                               : title + ": " + text;
            if (pretty.empty()) {
                send_error(resp, 400, "empty notification"); return;
            }
            // 2026-10-04: byte-truncate on a UTF-8 CHARACTER boundary —
            // the old resize(500) cut mid-character and produced poison
            // rows that killed dispatch with nlohmann type_error.316
            // (2026-10-02 poison-wake incident).
            if (pretty.size() > 500) {
                size_t cut = 500;
                while (cut > 0 &&
                       (static_cast<unsigned char>(pretty[cut]) & 0xC0) == 0x80)
                    --cut;
                pretty.resize(cut);
            }
            const std::string msg_id = threads.add_message(
                dm->id, "system", "hivemind",
                "[phone · " + app + "] " + pretty);
            send_json(resp, json{{"message_id", msg_id},
                                 {"thread_id", dm->id}}, 201);
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
