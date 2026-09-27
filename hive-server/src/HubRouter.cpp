// HubRouter (HiveMind 2.0 Phase C): pure-hub routing between
// roster/threads, workers, and the client. See HubRouter.h.

#include "hivemind/HubRouter.h"
#include "hivemind/RoutineStore.h"
#include "hivemind/Schedule.h"
#include "hivemind/LlmStore.h"
#include "hivemind/AgentStore.h"
#include "hivemind/ApprovalStore.h"
#include "hivemind/ContainerManager.h"
#include "hivemind/SecretStore.h"
#include "hivemind/ChatService.h"
#include "hivemind/DeliveryStore.h"
#include "hivemind/ThreadStore.h"
#include "hivemind/WorkerChannel.h"
#include "hivemind/AgentTools.h"

#include <filesystem>
#include <iostream>
#include <set>

using nlohmann::json;

HubRouter::HubRouter(AgentStore& agents, ThreadStore& threads,
                     WorkerChannel& channel, ApprovalStore& approvals,
                     RoutineStore& routines, hivemind::SecretStore& secrets)
    : agents_(agents), threads_(threads), channel_(channel),
      approvals_(approvals), routines_(routines), secrets_(secrets) {}

namespace {

/// Phase 53: same shape as RoutineRoutes' routine_json (that one is
/// file-local; duplicated here to keep the routes untouched).
json routine_to_json(const Routine& r) {
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
/// Phase 50: the agent's disabled-skill set — hub-known skills
/// (dirs under the server skills root) minus its enabled global-skills.
/// Rides the task frame so enable/disable goes live without a restart.
json disabled_skills_for(const std::string& agent_id) {
    json out = json::array();
    const auto enabled = agent_tools::list_enabled_skills(agent_id);
    const std::set<std::string> en(enabled.begin(), enabled.end());
    const auto root =
        AgentStore::agents_base_dir().parent_path() / "skills";
    std::error_code ec;
    for (const auto& e : std::filesystem::directory_iterator(root, ec)) {
        if (!e.is_directory()) continue;
        const auto name = e.path().filename().string();
        if (!en.count(name)) out.push_back(name);
    }
    return out;
}
}  // namespace

nlohmann::json secret_create_apply(hivemind::SecretStore& secrets,
                                   const std::string& name,
                                   const std::string& type,
                                   const nlohmann::json& payload,
                                   const std::string& agent_id) {
    if (!hivemind::SecretStore::valid_name(name))
        return {{"ok", false},
                {"error", "name must be [A-Za-z0-9_], start with a "
                          "letter/underscore, <=64 chars"}};
    if (!hivemind::SecretStore::valid_type(type))
        return {{"ok", false},
                {"error", "type must be api_key | login | note"}};
    // Required material per type (mirrors SecretRoutes validation).
    if (type == "login" &&
        payload.value("password", "").empty())
        return {{"ok", false}, {"error", "login requires a password"}};
    if (type == "note" && payload.value("text", "").empty())
        return {{"ok", false}, {"error", "note requires text"}};
    if (type == "api_key" && payload.value("value", "").empty())
        return {{"ok", false}, {"error", "api_key requires a value"}};
    if (secrets.exists(name))
        return {{"ok", false},
                {"error", "name taken — choose another (delete/recreate "
                          "is an admin action via the web UI)"}};
    if (!secrets.put_payload(name, type, payload))
        return {{"ok", false}, {"error", "store rejected the secret"}};
    // Auto-grant to the creator ONLY — sharing is a deliberate admin act.
    secrets.grant(name, agent_id);
    return {{"ok", true}, {"created", name}, {"type", type}};
}

nlohmann::json user_inbox_snapshot(ThreadStore& threads,
                                   AgentStore& agents, int limit) {
    nlohmann::json out = nlohmann::json::array();
    bool has_more = false;
    auto msgs = threads.list_messages_window(kUserThreadId, "", limit,
                                             has_more);
    for (const auto& m : msgs) {
        std::string who = m.sender_id;
        if (auto a = agents.get_agent(m.sender_id)) who = a->name;
        out.push_back({{"sender", who},
                       {"text", m.body},
                       {"created_at", m.created_at}});
    }
    return out;
}

std::string HubRouter::dispatch(const RosterAgent& agent,
                                const std::string& thread_id,
                                const std::string& text) {
    std::string task_id;
    {
        std::lock_guard<std::mutex> lk(m_);
        task_id = "t-" + std::to_string(next_task_++);
    }
    // Phase 40: carry the company roster with every task so agents can
    // discover each other (list_agents tool). Without it an agent told
    // to "message Forge" had no way to learn who exists and looped
    // trying to call a nonexistent directory tool (2026-09-07 incident).
    json roster = json::array();
    for (const auto& a : agents_.list_agents())
        roster.push_back({{"id", a.id}, {"name", a.name}});
    json op = {{"op", "task"},
               {"agent_id", agent.id},
               {"id", task_id},
               {"thread_id", thread_id},
               {"roster", roster},
               {"user_inbox", user_inbox_snapshot(threads_, agents_)},
               {"disabled_skills", disabled_skills_for(agent.id)},
               {"text", text}};
    // Phase 43: the hub owns provider/model config. Resolve the agent's
    // model ("" = hub default) and inject the full provider config into
    // the frame — the brain upserts it into its settings store before the
    // turn, so any model runs on any machine and config changes are live.
    if (llm_) {
        LlmModelInfo mi;
        auto prov = llm_->resolve(agent.model, &mi);
        if (prov) {
            op["llm"] = {{"provider", prov->name}, {"host", prov->host},
                         {"api_key", prov->api_key},
                         {"chat_endpoint", prov->chat_endpoint},
                         {"plugin", prov->plugin},
                         {"model", mi.name},
                         {"model_id", mi.model_id.empty() ? mi.name : mi.model_id},
                         {"max_context", mi.max_context},
                         {"temperature", mi.temperature}};
        } else {
            std::cerr << "[hub] dispatch: no LLM config for model '"
                      << (agent.model.empty() ? "(default)" : agent.model)
                      << "' — register providers/models first (agent "
                      << agent.name << ")\n";
            return "";
        }
    }
    if (!channel_.send(agent.worker, op)) {
        std::cerr << "[hub] dispatch failed: worker '" << agent.worker
                  << "' offline (agent " << agent.name << ")\n";
        return "";
    }
    std::lock_guard<std::mutex> lk(m_);
    tasks_[task_id] = {agent.id, agent.worker};
    return task_id;
}

void HubRouter::handle_routine_req(const std::string& worker,
                                   const std::string& agent_id,
                                   const json& frame) {
    const std::string id     = frame.value("id", "");
    const std::string action = frame.value("action", "");
    json resp = {{"op", "routine_resp"}, {"agent_id", agent_id}, {"id", id}};
    auto fail = [&](const std::string& err) {
        resp["ok"] = false;
        resp["error"] = err;
        channel_.send(worker, resp);
    };
    try {
        if (action == "list") {
            json arr = json::array();
            for (auto& r : routines_.list())
                if (r.agent_id == agent_id)
                    arr.push_back(routine_to_json(r));
            resp["ok"] = true;
            resp["result"] = {{"routines", arr}};
        } else if (action == "add") {
            const std::string name   = frame.value("name", "");
            const std::string prompt = frame.value("prompt", "");
            const json schedule = frame.value("schedule", json::object());
            if (name.empty() || prompt.empty())
                return fail("name and prompt required");
            if (auto err = hm::validate_schedule(schedule); !err.empty())
                return fail(err);
            // Same as the HTTP route: each routine gets its own thread so
            // fires don't spam DMs.
            auto t = threads_.create_thread("routine: " + name, "routine",
                                            {agent_id});
            auto next = hm::compute_next_fire(schedule, hm::now_iso_local(),
                                              hm::now_iso_local());
            auto r = routines_.create(agent_id, t.id, name, prompt,
                                      schedule.dump(), next.value_or(""));
            resp["ok"] = true;
            resp["result"] = routine_to_json(r);
        } else if (action == "update") {
            const std::string rid = frame.value("routine_id", "");
            auto r = routines_.get(rid);
            if (!r) return fail("routine not found");
            if (r->agent_id != agent_id)
                return fail("routine belongs to another agent");
            if (frame.contains("enabled"))
                routines_.set_enabled(rid, frame.value("enabled", true));
            if (frame.contains("name") || frame.contains("prompt") ||
                frame.contains("schedule")) {
                std::string name   = frame.value("name", r->name);
                std::string prompt = frame.value("prompt", r->prompt);
                std::string sched  = r->schedule_json;
                std::string next   = r->next_fire_at;
                if (frame.contains("schedule")) {
                    if (auto err = hm::validate_schedule(frame["schedule"]);
                        !err.empty())
                        return fail(err);
                    sched = frame["schedule"].dump();
                    next = hm::compute_next_fire(frame["schedule"],
                                                 hm::now_iso_local(),
                                                 r->created_at).value_or("");
                }
                routines_.update(rid, name, prompt, sched, next);
            }
            auto updated = routines_.get(rid);
            resp["ok"] = true;
            resp["result"] = updated ? routine_to_json(*updated)
                                     : json::object();
        } else if (action == "delete") {
            const std::string rid = frame.value("routine_id", "");
            auto r = routines_.get(rid);
            if (!r) return fail("routine not found");
            if (r->agent_id != agent_id)
                return fail("routine belongs to another agent");
            const std::string tid = r->thread_id;
            routines_.remove(rid);
            if (!tid.empty()) {
                try { threads_.delete_thread(tid); } catch (...) {}
            }
            resp["ok"] = true;
            resp["result"] = {{"deleted", rid}};
        } else {
            return fail("unknown routine action: " + action);
        }
    } catch (const std::exception& e) {
        return fail(e.what());
    }
    channel_.send(worker, resp);
}

// secrets_create bridge: an agent minting its own credential into the
// broker. The VALUE only ever lives inside `frame` here — it is never
// logged, never echoed in the response, and lands encrypted at rest
// (SecretStore, AES-256-GCM). Auto-granted to the creator only.
void HubRouter::handle_secret_req(const std::string& worker,
                                  const std::string& agent_id,
                                  const json& frame) {
    const std::string id = frame.value("id", "");
    json resp = {{"op", "secret_resp"}, {"agent_id", agent_id}, {"id", id}};
    try {
        const std::string action = frame.value("action", "");
        if (action != "create") {
            resp["ok"] = false;
            resp["error"] = "unknown action: " + action;
        } else {
            json r = secret_create_apply(
                secrets_, frame.value("name", ""),
                frame.value("type", "api_key"),
                frame.value("payload", json::object()), agent_id);
            resp["ok"] = r.value("ok", false);
            if (resp["ok"].get<bool>())
                resp["result"] = {{"created", r.value("created", "")},
                                  {"type", r.value("type", "")}};
            else
                resp["error"] = r.value("error", "create failed");
        }
    } catch (const std::exception& e) {
        resp["ok"] = false;
        resp["error"] = std::string{"secret_req failed: "} + e.what();
    }
    channel_.send(worker, resp);
}

void HubRouter::cancel_agent(const RosterAgent& agent,
                             const std::string& thread_id) {
    (void)thread_id;   // cancel is per-agent (hive-agent cancels its
                       // current turn; it runs one turn at a time)
    channel_.send(agent.worker,
                  {{"op", "cancel"}, {"agent_id", agent.id}});
}

void HubRouter::spawn_agent(const RosterAgent& agent) {
    if (!channel_.send(agent.worker, spawn_frame(agent)))
        throw std::runtime_error("worker '" + agent.worker +
                                 "' is offline");
    std::lock_guard<std::mutex> lk(m_);
    states_[agent.id] = "starting";
}

void HubRouter::stop_agent(const RosterAgent& agent) {
    channel_.send(agent.worker,
                  {{"op", "agent_stop"}, {"agent_id", agent.id}});
    std::lock_guard<std::mutex> lk(m_);
    states_[agent.id] = "down";
}

void HubRouter::remove_agent(const RosterAgent& agent) {
    channel_.send(agent.worker,
                  {{"op", "agent_remove"}, {"agent_id", agent.id}});
    std::lock_guard<std::mutex> lk(m_);
    states_.erase(agent.id);
}

json HubRouter::spawn_frame(const RosterAgent& agent) const {
    json mcp;
    try { mcp = json::parse(agent.mcp_servers); }
    catch (...) { mcp = json::array(); }
    // Phase 60: deliver GRANTED secrets with the spawn — the agent's
    // browser-fill tool reads HIVE_SECRET_<NAME> from its environment.
    // env_for_agent renders "NAME=value" lines (escaping handled there);
    // the worker writes them into the 0600 env file, nothing else.
    std::string secret_env;
    try {
        for (auto& [k, v] :
             ContainerManager::secret_store().env_for_agent(agent.id))
            secret_env += k + "=" + v + "\n";
    } catch (...) {}
    return {{"op", "agent_spawn"},
            {"agent_id", agent.id},
            {"name", agent.name},
            {"secret_env", secret_env},
            {"model", agent.model},          // NAME - worker resolves
            {"provider", ""},                // worker's models.json knows
            {"system_prompt", agent.role},
            {"auto_approve", !agent.require_approval},
            {"mcp_servers", mcp},
            {"image", ""},                  // "" = worker default (v2)
            {"type", agent.type},             // Phase 35: "" / "gpu"
            {"disabled_skills", disabled_skills_for(agent.id)}};  // Phase 50
}

void HubRouter::reconcile_worker(const std::string& worker) {
    for (const auto& a : agents_.list_agents()) {
        if (a.worker != worker) continue;
        if (!channel_.send(worker, spawn_frame(a)))
            return;   // went away mid-reconcile
        std::cerr << "[hub] agent_spawn -> " << worker << ": "
                  << a.name << "\n";
    }
}

void HubRouter::on_heartbeat(const std::string& worker,
                             const json& agents) {
    if (!agents.is_object()) return;
    std::lock_guard<std::mutex> lk(m_);
    for (auto it = agents.begin(); it != agents.end(); ++it)
        if (it.value().is_string()) states_[it.key()] = it.value().get<std::string>();
    (void)worker;
}

void HubRouter::on_worker_frame(const std::string& worker,
                                const json& frame) {
    const std::string op = frame.value("op", "");
    const std::string agent_id = frame.value("agent", "");

    if (op == "task_delta") {
        if (chat_) chat_->append_delta(frame.value("task", ""),
                                       frame.value("text", ""));
        return;
    }
    if (op == "routine_req") {   // Phase 53
        handle_routine_req(worker, agent_id, frame);
        return;
    }
    if (op == "secret_req") {    // secrets_create tool bridge
        handle_secret_req(worker, agent_id, frame);
        return;
    }
    if (op == "spawn_req") {    // Phase 65: worker crash-respawned from a
        // possibly-stale cached spec — push a fresh spawn frame (freshly
        // rendered secret_env etc.). The worker no-ops unchanged specs.
        if (auto a = agents_.get_agent(agent_id)) {
            try {
                spawn_agent(*a);
            } catch (const std::exception& e) {
                std::cerr << "[hub] spawn_req for " << agent_id
                          << " failed: " << e.what() << "\n";
            }
        }
        return;
    }
    if (op == "task_result") {
        const std::string task = frame.value("task", "");
        {
            std::lock_guard<std::mutex> lk(m_);
            tasks_.erase(task);
        }
        // Phase 35: worker ships attachment records minus the worker
        // field (it doesn't know its hub-side name) — inject it here so
        // clients can fetch via /api/files/download?worker&path.
        // NOTE: the worker sends a JSON ARRAY (legacy peers send a
        // string) — value("attachments","[]") would throw type_error on
        // an array and silently drop the whole task_result.
        std::string atts = "[]";
        if (frame.contains("attachments")) {
            const auto& a = frame["attachments"];
            atts = a.is_string() ? a.get<std::string>() : a.dump();
        }
        {
            auto arr = json::parse(atts, nullptr, false);
            if (!arr.is_discarded() && arr.is_array() && !arr.empty()) {
                for (auto& a : arr) a["worker"] = worker;
                atts = arr.dump();
            }
        }
        // Phase 64: stats object (agent → worker passthrough). Same
        // type-guard discipline as attachments above — dump if object.
        std::string stats;
        if (frame.contains("stats") && frame["stats"].is_object())
            stats = frame["stats"].dump();
        if (chat_) chat_->complete_turn(task, frame.value("body", ""),
                                        frame.value("error", ""), atts,
                                        frame.value("thread_id", ""),
                                        agent_id, stats);
        return;
    }
    if (op == "approval_req") {
        // Register a PENDING approval the client can see and decide; the
        // worker-side request id is remembered for the response relay.
        auto agent = agents_.get_agent(agent_id);
        const std::string store_id = approvals_.create(
            agent_id, agent ? agent->name : agent_id,
            "command", frame.value("command", ""));
        {
            std::lock_guard<std::mutex> lk(m_);
            pend_appr_[store_id] = {worker, agent_id,
                                    frame.value("id", "")};
        }
        std::cerr << "[hub] approval_req " << store_id << " from "
                  << agent_id << ": "
                  << frame.value("command", "").substr(0, 80) << "\n";
        return;
    }
    if (op == "agent_status") {
        const std::string st = frame.value("state", "");
        {
            std::lock_guard<std::mutex> lk(m_);
            states_[agent_id] = st;
        }
        // 2026-09-25 busy-receiver hold queue: a spawn/reconnect "ready"
        // is the recovery drain hook for wakes held across a restart.
        // Called OUTSIDE m_ — draining re-enters dispatch (lock order).
        if (chat_ && st == "ready") chat_->on_agent_ready(agent_id);
        return;
    }
    if (op == "msg_send_req") {   // Phase 66d: confirmed message_agent
        handle_msg_send_req(worker, agent_id, frame);
        return;
    }
    if (op == "msg_status_req") { // Phase 66d: message_status tool
        handle_msg_status_req(worker, agent_id, frame);
        return;
    }
    if (op == "agent_msg") {
        // hive-agent's message_agent tool -> pair thread (async, v1 design)
        if (chat_) {
            // target may be a roster id OR a display name (agents know
            // each other by name) - resolve here.
            std::string target = frame.value("target", "");
            if (!agents_.get_agent(target)) {
                if (auto by_name = agents_.get_agent_by_name(target))
                    target = by_name->id;
            }
            // Phase 44: an agent addressing the human via message_agent
            // (target "user"/"human"/unresolvable) is REDIRECTED to the
            // User Inbox instead of failing. Before this, agents trying
            // to PM the user improvised into group threads, fanned in to
            // every participant and looped endlessly (2026-09-09).
            if (!agents_.get_agent(target)) {
                try {
                    chat_->post_user_inbox_message(agent_id,
                                                   frame.value("text", ""));
                    std::cerr << "[hub] agent_msg target '" << target
                              << "' unresolved -> user inbox\n";
                } catch (const std::exception& e) {
                    std::cerr << "[hub] agent_msg->inbox failed: "
                              << e.what() << "\n";
                }
                return;
            }
            try {
                chat_->post_agent_message(agent_id, target,
                                          frame.value("text", ""));
            } catch (const std::exception& e) {
                std::cerr << "[hub] agent_msg failed: " << e.what() << "\n";
            }
        }
        return;
    }
    if (op == "user_msg") {
        // Phase 44: hive-agent's message_user tool -> User Inbox thread.
        // One-way notification to the human; never fans in.
        if (chat_) {
            try {
                chat_->post_user_inbox_message(agent_id,
                                               frame.value("text", ""));
            } catch (const std::exception& e) {
                std::cerr << "[hub] user_msg failed: " << e.what() << "\n";
            }
        }
        return;
    }
    if (op == "agent_log") {
        std::cerr << "[hub] " << agent_id << ": " << frame.dump() << "\n";
        return;
    }
    // Unknown ops fall through silently (forward-compatible).
}

bool HubRouter::resolve_approval(const std::string& id, bool allow) {
    PendAppr pa;
    {
        std::lock_guard<std::mutex> lk(m_);
        auto it = pend_appr_.find(id);
        if (it == pend_appr_.end()) return false;
        pa = it->second;
        pend_appr_.erase(it);
    }
    approvals_.resolve(id, allow);
    return channel_.send(pa.worker, {{"op", "approval_resp"},
                                     {"agent_id", pa.agent_id},
                                     {"id", pa.req_id},
                                     {"allow", allow}});
}

std::string HubRouter::agent_state(const std::string& agent_id) const {
    // A remembered state is only meaningful while the agent's worker is
    // connected. states_ survives restarts and disconnects (persisted
    // heartbeats), so without this check an agent on an offline rig shows
    // its last state ("ready") forever -- Bob on a powered-off dev-rig
    // looked ready (2026-08-31). Offline worker = down, period; when the
    // rig reconnects, heartbeats refresh states_ with ground truth.
    auto agent = agents_.get_agent(agent_id);
    if (agent && !channel_.is_online(agent->worker)) return "down";
    std::lock_guard<std::mutex> lk(m_);
    auto it = states_.find(agent_id);
    return it != states_.end() ? it->second : "";
}

// ── Phase 66d: confirmed message_agent + message_status ───────────────

namespace {
json delivery_to_json(const hivemind::Delivery& d, bool read) {
    return {{"id", d.id},
            {"kind", d.kind},
            {"thread_id", d.thread_id},
            {"target_agent_id", d.target_agent_id},
            {"sender_agent_id", d.sender_agent_id},
            {"state", d.state},
            {"read", read},
            {"error", d.error},
            {"created_at", d.created_at},
            {"dispatched_at", d.dispatched_at},
            {"completed_at", d.completed_at}};
}
}  // namespace

void HubRouter::handle_msg_send_req(const std::string& worker,
                                    const std::string& agent_id,
                                    const json& frame) {
    const std::string id = frame.value("id", "");
    json resp = {{"op", "msg_send_resp"}, {"agent_id", agent_id},
                 {"id", id}};
    if (!chat_) {
        resp["ok"] = false;
        resp["error"] = "chat service unavailable";
        channel_.send(worker, resp);
        return;
    }
    // Same target resolution as the legacy agent_msg path: roster id OR
    // display name; unresolvable -> redirected to the User Inbox.
    std::string target = frame.value("target", "");
    if (!agents_.get_agent(target)) {
        if (auto by_name = agents_.get_agent_by_name(target))
            target = by_name->id;
    }
    try {
        if (!agents_.get_agent(target)) {
            chat_->post_user_inbox_message(agent_id,
                                           frame.value("text", ""));
            resp["ok"] = true;
            resp["redirected_to_user_inbox"] = true;
            std::cerr << "[hub] msg_send target '" << target
                      << "' unresolved -> user inbox\n";
        } else {
            std::string delivery_id;
            const std::string tid = chat_->post_agent_message(
                agent_id, target, frame.value("text", ""), &delivery_id);
            resp["ok"] = true;
            resp["thread_id"] = tid;
            resp["delivery_id"] = delivery_id;
            resp["state"] = "queued";
        }
    } catch (const std::exception& e) {
        resp["ok"] = false;
        resp["error"] = e.what();
    }
    channel_.send(worker, resp);
}

void HubRouter::handle_msg_status_req(const std::string& worker,
                                      const std::string& agent_id,
                                      const json& frame) {
    const std::string id = frame.value("id", "");
    json resp = {{"op", "msg_status_resp"}, {"agent_id", agent_id},
                 {"id", id}};
    if (!chat_) {
        resp["ok"] = false;
        resp["error"] = "chat service unavailable";
        channel_.send(worker, resp);
        return;
    }
    try {
        json arr = json::array();
        for (const auto& d : chat_->delivery_status(
                 agent_id, frame.value("delivery_id", "")))
            arr.push_back(delivery_to_json(d, chat_->delivery_is_read(d)));
        resp["ok"] = true;
        resp["deliveries"] = std::move(arr);
    } catch (const std::exception& e) {
        resp["ok"] = false;
        resp["error"] = e.what();
    }
    channel_.send(worker, resp);
}
