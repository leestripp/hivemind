// HubRouter (HiveMind 2.0 Phase C): pure-hub routing between
// roster/threads, workers, and the client. See HubRouter.h.

#include "hivemind/HubRouter.h"
#include "hivemind/AgentStore.h"
#include "hivemind/ApprovalStore.h"
#include "hivemind/ChatService.h"
#include "hivemind/ThreadStore.h"
#include "hivemind/WorkerChannel.h"

#include <iostream>

using nlohmann::json;

HubRouter::HubRouter(AgentStore& agents, ThreadStore& threads,
                     WorkerChannel& channel, ApprovalStore& approvals)
    : agents_(agents), threads_(threads), channel_(channel),
      approvals_(approvals) {}

std::string HubRouter::dispatch(const RosterAgent& agent,
                                const std::string& thread_id,
                                const std::string& text) {
    std::string task_id;
    {
        std::lock_guard<std::mutex> lk(m_);
        task_id = "t-" + std::to_string(next_task_++);
    }
    json op = {{"op", "task"},
               {"agent_id", agent.id},
               {"id", task_id},
               {"thread_id", thread_id},
               {"text", text}};
    if (!channel_.send(agent.worker, op)) {
        std::cerr << "[hub] dispatch failed: worker '" << agent.worker
                  << "' offline (agent " << agent.name << ")\n";
        return "";
    }
    std::lock_guard<std::mutex> lk(m_);
    tasks_[task_id] = {agent.id, agent.worker};
    return task_id;
}

void HubRouter::cancel_agent(const RosterAgent& agent,
                             const std::string& thread_id) {
    (void)thread_id;   // cancel is per-agent (hive-agent cancels its
                       // current turn; it runs one turn at a time)
    channel_.send(agent.worker,
                  {{"op", "cancel"}, {"agent_id", agent.id}});
}

void HubRouter::stop_agent(const RosterAgent& agent) {
    channel_.send(agent.worker,
                  {{"op", "agent_stop"}, {"agent_id", agent.id}});
    std::lock_guard<std::mutex> lk(m_);
    states_[agent.id] = "down";
}

json HubRouter::spawn_frame(const RosterAgent& agent) const {
    json mcp;
    try { mcp = json::parse(agent.mcp_servers); }
    catch (...) { mcp = json::array(); }
    return {{"op", "agent_spawn"},
            {"agent_id", agent.id},
            {"name", agent.name},
            {"model", agent.model},          // NAME - worker resolves
            {"provider", ""},                // worker's models.json knows
            {"system_prompt", agent.role},
            {"auto_approve", !agent.require_approval},
            {"mcp_servers", mcp},
            {"image", ""}};                  // "" = worker default (v2)
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
    if (op == "task_result") {
        const std::string task = frame.value("task", "");
        {
            std::lock_guard<std::mutex> lk(m_);
            tasks_.erase(task);
        }
        if (chat_) chat_->complete_turn(task, frame.value("body", ""),
                                        frame.value("error", ""),
                                        frame.value("attachments", "[]"));
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
        std::lock_guard<std::mutex> lk(m_);
        states_[agent_id] = frame.value("state", "");
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
            try {
                chat_->post_agent_message(agent_id, target,
                                          frame.value("text", ""));
            } catch (const std::exception& e) {
                std::cerr << "[hub] agent_msg failed: " << e.what() << "\n";
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
    std::lock_guard<std::mutex> lk(m_);
    auto it = states_.find(agent_id);
    return it != states_.end() ? it->second : "";
}
