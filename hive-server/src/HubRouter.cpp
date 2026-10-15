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
#include "hivemind/ShareStore.h"

#include <filesystem>
#include <iostream>
#include <set>
#include <thread>

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
            // 2026-10-01: effective thinking level — the agent's override
            // wins, else the model's own setting ("" = provider default).
            // There is no generic provider discovery for this, so both are
            // editor fields (Plan.md); the plugin ignores unsupported ones.
            const std::string thinking =
                agent.thinking_level.empty() ? mi.thinking_level
                                             : agent.thinking_level;
            op["llm"] = {{"provider", prov->name}, {"host", prov->host},
                         {"api_key", prov->api_key},
                         {"chat_endpoint", prov->chat_endpoint},
                         {"plugin", prov->plugin},
                         {"model", mi.name},
                         {"model_id", mi.model_id.empty() ? mi.name : mi.model_id},
                         {"max_context", mi.max_context},
                         {"temperature", mi.temperature},
                         {"thinking_level", thinking}};
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
    // The create auto-granted to this agent — the grant write is DB-only,
    // so push a fresh spawn frame AFTER the resp or the agent never sees
    // its own secret in env (2026-09-29 vault-loss incident). The fleet
    // restarts the brain on the spec change; that is the designed
    // grant-propagation semantic (fleet.cpp vault-grant comment).
    // Phase 67.2: push_spawn defers while the agent is mid-turn — an
    // immediate respawn here killed the very turn that created the
    // secret (task_result lost, hub turn wedged until cancel/timeout).
    if (resp["ok"].get<bool>())
        push_spawn(agent_id);
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

void HubRouter::push_spawn(const std::string& agent_id) {
    if (chat_ && chat_->agent_busy(agent_id)) {
        std::lock_guard<std::mutex> lk(m_);
        if (pending_spawn_.insert(agent_id).second)
            std::cerr << "[hub] spawn push for " << agent_id
                      << " deferred — agent mid-turn\n";
        return;
    }
    if (auto a = agents_.get_agent(agent_id)) {
        try { spawn_agent(*a); }
        catch (const std::exception& e) {
            std::cerr << "[hub] spawn push for " << agent_id
                      << " failed: " << e.what() << "\n";
        }
    }
}

void HubRouter::flush_pending_spawns() {
    std::vector<std::string> todo;
    {
        std::lock_guard<std::mutex> lk(m_);
        if (pending_spawn_.empty()) return;
        todo.assign(pending_spawn_.begin(), pending_spawn_.end());
    }
    for (const auto& id : todo) {
        if (chat_ && chat_->agent_busy(id)) continue;   // still turning
        {
            std::lock_guard<std::mutex> lk(m_);
            pending_spawn_.erase(id);
        }
        std::cerr << "[hub] flushing deferred spawn push for " << id << "\n";
        if (auto a = agents_.get_agent(id)) {
            try { spawn_agent(*a); }
            catch (const std::exception& e) {
                std::cerr << "[hub] deferred spawn push for " << id
                          << " failed: " << e.what() << "\n";
            }
        }
    }
}

void HubRouter::stop_agent(const RosterAgent& agent) {
    channel_.send(agent.worker,
                  {{"op", "agent_stop"}, {"agent_id", agent.id}});
    std::lock_guard<std::mutex> lk(m_);
    states_[agent.id] = "down";
}

void HubRouter::remove_agent(const RosterAgent& agent) {
    // Phase 72: host_agent + name let the worker auto-deprovision the
    // unix user on roster delete (workspace never touched).
    channel_.send(agent.worker,
                  {{"op", "agent_remove"}, {"agent_id", agent.id},
                   {"host_agent", agent.host_agent}, {"name", agent.name}});
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
            {"memory", agent.memory},         // Phase 69: "" = worker default
            {"cpus", agent.cpus},             // Phase 69: 0 = worker default
            // Phase 72: host agent — the worker independently verifies its
            // own allow_host_agents opt-in before honoring this (defense in
            // depth; the route-side caps gate is in RosterRoutes).
            {"host_agent", agent.host_agent},
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
    flush_pending_spawns();
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
    if (op == "task_attachment") {
        // 2026-10-15 (Plan.md): attach_file shipped a file MID-TURN.
        // Post it immediately so it survives a turn interrupt; same
        // worker-injection + type-guard discipline as task_result below
        // (worker sends a JSON ARRAY; legacy peers may send a string).
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
            } else {
                return;   // nothing usable — drop
            }
        }
        if (chat_) chat_->post_turn_attachment(
            frame.value("task", ""), frame.value("thread_id", ""),
            agent_id, atts);
        return;
    }
    if (op == "task_result") {
        const std::string task = frame.value("task", "");
        {
            std::lock_guard<std::mutex> lk(m_);
            tasks_.erase(task);
        }
        flush_pending_spawns();   // a turn ended — deferred pushes go now
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
    if (op == "approval_resolved") {
        // The agent settled this approval itself (cancel/shutdown deny
        // or the broker safety timeout) — clear the pending row so the
        // client doesn't show ghosts. No approval_resp relay: the agent
        // already knows.
        const std::string agent_id = frame.value("agent", "");
        const std::string req_id = frame.value("id", "");
        std::string store_id;
        {
            std::lock_guard<std::mutex> lk(m_);
            for (auto it = pend_appr_.begin(); it != pend_appr_.end(); ++it)
                if (it->second.agent_id == agent_id &&
                    it->second.req_id == req_id) {
                    store_id = it->first;
                    pend_appr_.erase(it);
                    break;
                }
        }
        if (!store_id.empty()) {
            approvals_.resolve(store_id, frame.value("allow", false));
            std::cerr << "[hub] approval " << store_id
                      << " settled agent-side (deny)\n";
        }
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
    if (op == "share_create_req") {   // share_file tool (2026-10-11)
        handle_share_create_req(worker, agent_id, frame);
        return;
    }
    if (op == "share_list_req") {
        handle_share_list_req(worker, agent_id, frame);
        return;
    }
    if (op == "share_fetch_req") {
        handle_share_fetch_req(worker, agent_id, frame);
        return;
    }
    if (op == "share_revoke_req") {
        handle_share_revoke_req(worker, agent_id, frame);
        return;
    }
    if (op == "share_copy_ack") {   // same-worker fetch completion
        handle_share_copy_ack(worker, frame);
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
    // 2026-09-29: ground truth beats self-report. The agent's own
    // set_status tool can declare "Done"/"Idle" MID-TURN (Scout did this
    // habitually) and the relay then overwrites the worker's accurate
    // "acting" — the webUI showed Idle while her turn was still running
    // ("PA shows Idle but she is still doing something"). A live turn
    // means busy, whatever the model claims; self-report is only trusted
    // when no turn is in flight.
    if (chat_ && chat_->agent_busy(agent_id)) return "acting";
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

// =============================================================================
// share_file tool bridge (2026-10-11, Plan.md "Agent-to-Agent Secure File
// Sharing"). Addressed shares: the worker stages the blob at
// workspace/shared/<share_id>/<name> BEFORE calling us (path confinement,
// 50 MB cap and the uuid are worker-side); the hub stores only metadata and
// enforces fetch authorization. Fetches relay through the existing chunked
// file_fetch/file_push ops — a same-worker fetch never leaves the rig
// (share_local_copy op), a cross-worker fetch transits hub memory once.
// =============================================================================

namespace {

constexpr int64_t kMaxShareBytes = 50LL * 1024 * 1024;

/// UTF-8-safe text cap: never split a multi-byte character (the
/// 2026-10-02 poison-wake lesson — byte truncation made a wake body
/// that nlohmann could not serialize).
std::string utf8_safe_prefix(const std::string& s, std::size_t max_bytes) {
    if (s.size() <= max_bytes) return s;
    std::size_t n = max_bytes;
    while (n > 0 &&
           (static_cast<unsigned char>(s[n]) & 0xC0) == 0x80)
        --n;
    return s.substr(0, n) + "...";
}

std::string fmt_bytes(int64_t n) {
    if (n >= 1024 * 1024)
        return std::to_string(n / (1024 * 1024)) + " MB";
    if (n >= 1024)
        return std::to_string(n / 1024) + " KB";
    return std::to_string(n) + " B";
}

}  // namespace

void HubRouter::handle_share_create_req(const std::string& worker,
                                        const std::string& agent_id,
                                        const json& frame) {
    const std::string id = frame.value("id", "");
    json resp = {{"op", "share_create_resp"}, {"agent_id", agent_id},
                 {"id", id}};
    auto fail = [&](const std::string& err) {
        resp["ok"] = false;
        resp["error"] = err;
        channel_.send(worker, resp);
    };
    if (!shares_) { fail("share store unavailable"); return; }
    if (!chat_)   { fail("chat service unavailable"); return; }
    const std::string share_id = frame.value("share_id", "");
    const std::string filename = frame.value("filename", "");
    const std::string rel_path = frame.value("rel_path", "");
    const int64_t size = frame.value("size", static_cast<int64_t>(0));
    if (share_id.empty() || filename.empty() || rel_path.empty()) {
        fail("missing share fields"); return;
    }
    if (size <= 0) { fail("empty file"); return; }
    if (size > kMaxShareBytes) {   // defense in depth (worker enforces too)
        fail("file too large (50 MB max)"); return;
    }
    // The staged copy must live under the shared/ staging dir.
    if (rel_path.find("..") != std::string::npos ||
        rel_path.rfind("shared/", 0) != 0) {
        fail("bad staging path"); return;
    }
    const auto sender = agents_.get_agent(agent_id);
    if (!sender) { fail("unknown sender"); return; }
    // Identity pinning: the sender must be homed on the connection this
    // request arrived on (same trust rule as msg_send_req's agent stamp).
    if (sender->worker != worker) {
        fail("sender not homed on this worker"); return;
    }
    // Target resolution mirrors handle_msg_send_req: roster id OR
    // display name. Files are NOT redirected to the user inbox — an
    // unresolvable target is an error (the user path is attachments).
    std::string target = frame.value("target", "");
    if (!agents_.get_agent(target)) {
        if (auto by_name = agents_.get_agent_by_name(target))
            target = by_name->id;
    }
    if (!agents_.get_agent(target)) {
        fail("target not found: " + frame.value("target", "")); return;
    }
    if (target == agent_id) { fail("cannot share with yourself"); return; }

    hivemind::Share sh;
    sh.id              = share_id;
    sh.sender_agent_id = agent_id;
    sh.target_agent_id = target;
    sh.filename        = filename;
    sh.size_bytes      = size;
    sh.note            = utf8_safe_prefix(frame.value("note", ""), 512);
    sh.worker          = worker;
    sh.rel_path        = rel_path;
    try {
        shares_->create_share(sh);
    } catch (const std::exception& e) {
        fail(std::string{"store: "} + e.what()); return;
    }

    // Notify the target through the durable Phase 66 wake path. A
    // notification failure must not kill the share — the row exists and
    // list_shared_files still surfaces it.
    const std::string body =
        "shared a file with you: " + filename + " (" + fmt_bytes(size) +
        ") — fetch it with fetch_shared_file(\"" + share_id + "\")" +
        (sh.note.empty() ? "" : "\nNote: " + sh.note);
    try {
        std::string delivery_id;
        chat_->post_agent_message(agent_id, target, body, &delivery_id);
    } catch (const std::exception& e) {
        resp["notify_error"] = e.what();
        std::cerr << "[hub] share " << share_id
                  << " notification failed: " << e.what() << "\n";
    }
    resp["ok"] = true;
    resp["share_id"] = share_id;
    std::cerr << "[hub] share " << share_id << ": " << sender->name
              << " -> " << target << " " << filename << " ("
              << fmt_bytes(size) << ")\n";
    channel_.send(worker, resp);
}

void HubRouter::handle_share_list_req(const std::string& worker,
                                      const std::string& agent_id,
                                      const json& frame) {
    const std::string id = frame.value("id", "");
    json resp = {{"op", "share_list_resp"}, {"agent_id", agent_id},
                 {"id", id}};
    if (!shares_) {
        resp["ok"] = false;
        resp["error"] = "share store unavailable";
        channel_.send(worker, resp);
        return;
    }
    try {
        json arr = json::array();
        for (const auto& sh : shares_->list_for_agent(agent_id, 50)) {
            json j = hivemind::share_to_json(sh);
            j["direction"] =
                sh.sender_agent_id == agent_id ? "sent" : "received";
            if (const auto peer = agents_.get_agent(
                    j["direction"] == "sent" ? sh.target_agent_id
                                             : sh.sender_agent_id))
                j["peer_name"] = peer->name;
            arr.push_back(std::move(j));
        }
        resp["ok"] = true;
        resp["shares"] = std::move(arr);
    } catch (const std::exception& e) {
        resp["ok"] = false;
        resp["error"] = e.what();
    }
    channel_.send(worker, resp);
}

void HubRouter::handle_share_fetch_req(const std::string& worker,
                                       const std::string& agent_id,
                                       const json& frame) {
    const std::string id = frame.value("id", "");
    json resp = {{"op", "share_fetch_resp"}, {"agent_id", agent_id},
                 {"id", id}};
    auto fail = [&](const std::string& err) {
        resp["ok"] = false;
        resp["error"] = err;
        channel_.send(worker, resp);
    };
    if (!shares_) { fail("share store unavailable"); return; }
    const std::string share_id = frame.value("share_id", "");
    const auto sh = shares_->get_share(share_id);
    if (!sh) { fail("share not found"); return; }
    if (sh->state != "active") { fail("share was revoked"); return; }
    // Addressed-share authorization: only the target (or the sender
    // re-fetching their own share) may pull the blob.
    if (sh->target_agent_id != agent_id && sh->sender_agent_id != agent_id) {
        fail("share is not addressed to you"); return;
    }

    if (sh->worker == worker) {
        // Same-rig fast path: the worker copies the staged file into
        // incoming/ in place — a 50 MB blob must not round-trip through
        // the hub (WAN, memory) when both agents share the workspace.
        {
            std::lock_guard<std::mutex> lk(m_);
            pend_fetch_[share_id] = {worker, agent_id, id};
        }
        const bool sent = channel_.send(worker,
            {{"op", "share_local_copy"},
             {"id", share_id},
             {"src", sh->rel_path},
             {"dst", "incoming/" + share_id + "/" + sh->filename}});
        if (!sent) {
            std::lock_guard<std::mutex> lk(m_);
            pend_fetch_.erase(share_id);
            fail("worker went offline");
        }
        return;   // the response rides share_copy_ack
    }

    // Cross-worker relay. fetch_file/push_file are synchronous (up to
    // 120 s each) — never run them on the channel read loop.
    if (!channel_.is_online(sh->worker)) {
        fail("sender worker offline"); return;
    }
    const hivemind::Share snap = *sh;
    std::thread([this, worker, agent_id, id, snap] {
        json r = {{"op", "share_fetch_resp"}, {"agent_id", agent_id},
                  {"id", id}};
        try {
            auto fr = channel_.fetch_file(snap.worker, "", snap.rel_path);
            if (!fr) {
                r["ok"] = false;
                r["error"] = "sender worker went offline";
            } else if (!fr->ok) {
                r["ok"] = false;
                r["error"] = fr->error;
            } else {
                const std::string dst =
                    "incoming/" + snap.id + "/" + snap.filename;
                auto pr = channel_.push_file(worker, dst, fr->data);
                if (!pr) {
                    r["ok"] = false;
                    r["error"] = "worker went offline";
                } else if (!pr->ok) {
                    r["ok"] = false;
                    r["error"] = pr->error;
                } else {
                    shares_->mark_fetched(snap.id);
                    r["ok"] = true;
                    r["path"] = "/workspace/" + dst;
                    r["name"] = snap.filename;
                    r["size"] = snap.size_bytes;
                }
            }
        } catch (const std::exception& e) {
            r["ok"] = false;
            r["error"] = e.what();
        } catch (...) {
            r["ok"] = false;
            r["error"] = "relay failed";
        }
        try { channel_.send(worker, r); }
        catch (const std::exception& e) {
            std::cerr << "[hub] share_fetch_resp send failed: "
                      << e.what() << "\n";
        }
    }).detach();
}

void HubRouter::handle_share_copy_ack(
    const std::string& /*worker*/,   // response relays via pf.worker
    const json& frame) {
    const std::string share_id = frame.value("id", "");
    PendFetch pf;
    {
        std::lock_guard<std::mutex> lk(m_);
        const auto it = pend_fetch_.find(share_id);
        if (it == pend_fetch_.end()) return;   // stale/unknown ack
        pf = it->second;
        pend_fetch_.erase(it);
    }
    json resp = {{"op", "share_fetch_resp"}, {"agent_id", pf.agent_id},
                 {"id", pf.req_id}};
    if (!frame.value("ok", false)) {
        resp["ok"] = false;
        resp["error"] = frame.value("error", "local copy failed");
    } else {
        const auto sh = shares_ ? shares_->get_share(share_id)
                                : std::nullopt;
        if (shares_ && sh) shares_->mark_fetched(share_id);
        resp["ok"] = true;
        resp["path"] = "/workspace/incoming/" + share_id + "/" +
                       (sh ? sh->filename : "");
        if (sh) {
            resp["name"] = sh->filename;
            resp["size"] = sh->size_bytes;
        }
    }
    channel_.send(pf.worker, resp);
}

void HubRouter::handle_share_revoke_req(const std::string& worker,
                                        const std::string& agent_id,
                                        const json& frame) {
    const std::string id = frame.value("id", "");
    json resp = {{"op", "share_revoke_resp"}, {"agent_id", agent_id},
                 {"id", id}};
    auto fail = [&](const std::string& err) {
        resp["ok"] = false;
        resp["error"] = err;
        channel_.send(worker, resp);
    };
    if (!shares_) { fail("share store unavailable"); return; }
    const std::string share_id = frame.value("share_id", "");
    const auto sh = shares_->get_share(share_id);
    if (!sh) { fail("share not found"); return; }
    if (sh->sender_agent_id != agent_id) {
        fail("only the sender can revoke a share"); return;
    }
    try {
        shares_->revoke(share_id);   // already-revoked is still success
    } catch (const std::exception& e) {
        fail(std::string{"store: "} + e.what()); return;
    }
    // Best-effort: drop the staged copy on the sender's worker. The
    // share is revoked either way — a leftover file just wastes disk
    // until the worker's next cleanup.
    if (channel_.is_online(sh->worker))
        channel_.send(sh->worker, {{"op", "share_delete"},
                                   {"path", sh->rel_path}});
    resp["ok"] = true;
    channel_.send(worker, resp);
}
