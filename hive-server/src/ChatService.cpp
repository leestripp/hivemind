// ChatService (HiveMind 2.0 Phase C): hub-side thread<->agent glue.
// Turns run on workers; this class posts messages, dispatches tasks via
// HubRouter, and completes live turns when task_result frames arrive.

#include "hivemind/ChatService.h"
#include "hivemind/HubRouter.h"
#include "hivemind/AgentStore.h"
#include "hivemind/ThreadStore.h"
#include "hivemind/ContainerManager.h"
#include "hivemind/ExecAuditStore.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdio>
#include <stdexcept>

namespace {

// "[Attached files ...]" reference list appended to the agent prompt; the
// stored message body stays clean (client renders chips from the JSON).
std::string attachment_refs(const std::string& attachments_json) {
    if (attachments_json.empty() || attachments_json == "[]") return "";
    auto atts = nlohmann::json::parse(attachments_json, nullptr, false);
    if (!atts.is_array() || atts.empty()) return "";
    std::string out = "\n\n[Attached files in the shared workspace:";
    bool any = false;
    for (auto& a : atts) {
        const std::string p = a.value("path", "");
        if (p.empty()) continue;
        out += "\n- " + p;
        any = true;
    }
    if (!any) return "";
    return out + "]\n";
}

} // namespace

std::string ChatService::post_agent_message(const std::string& sender_id,
                                            const std::string& target_id,
                                            const std::string& body) {
    if (stopping_)
        throw std::invalid_argument("server is shutting down");
    auto sender = agents_.get_agent(sender_id);
    auto target = agents_.get_agent(target_id);
    if (!sender || !target)
        throw std::invalid_argument("unknown sender or target agent");

    // Find-or-create the pair thread (kind "group", exactly these two).
    std::string tid;
    for (const auto& t : threads_.list_threads()) {
        if (t.kind == "group" && t.agent_ids.size() == 2 &&
            std::find(t.agent_ids.begin(), t.agent_ids.end(), sender_id) !=
                t.agent_ids.end() &&
            std::find(t.agent_ids.begin(), t.agent_ids.end(), target_id) !=
                t.agent_ids.end()) {
            tid = t.id;
            break;
        }
    }
    if (tid.empty()) {
        auto t = threads_.create_thread(
            sender->name + " <-> " + target->name, "group",
            {sender_id, target_id});
        tid = t.id;
    }

    threads_.add_message(tid, "agent", sender_id, body);
    fan_in(tid, *target, sender->name, body, /*generation=*/1);
    return tid;
}

void ChatService::fan_in(const std::string& thread_id,
                         const RosterAgent& agent,
                         const std::string& sender_label,
                         const std::string& body,
                         int generation) {
    // The agent's daedalus conversation (persisted on the worker's
    // /workspace volume) IS the context - each task carries ONLY the new
    // message. DO NOT add a thread-history backlog here (v1 lesson).
    std::string prompt = "[" + sender_label + "] " + body;

    auto live = std::make_shared<LiveTurn>();
    live->thread_id  = thread_id;
    live->agent_id   = agent.id;
    live->agent_name = agent.name;
    live->worker     = agent.worker;
    live->generation = generation;

    const std::string task_id = hub_.dispatch(agent, thread_id, prompt);
    {
        std::lock_guard<std::mutex> lk(m_);
        auto& vec = live_[thread_id];
        vec.erase(std::remove_if(vec.begin(), vec.end(),
                                 [](const auto& t) { return t->done; }),
                  vec.end());
        if (vec.size() >= kMaxLivePerThread)
            vec.erase(vec.begin());
        vec.push_back(live);
        if (task_id.empty()) {
            live->error = "worker offline";
            live->done = true;
        } else {
            live->task_id = task_id;
            by_task_[task_id] = live;
        }
    }
}

std::string ChatService::post_user_message(const std::string& thread_id,
                                           const std::string& body,
                                           const std::string& attachments_json) {
    if (stopping_)
        throw std::invalid_argument("server is shutting down");
    auto thread = threads_.get_thread(thread_id);
    if (!thread)
        throw std::invalid_argument("thread not found: " + thread_id);

    std::string msg_id =
        threads_.add_message(thread_id, "user", "", body, attachments_json);

    const std::string prompt_body = body + attachment_refs(attachments_json);

    for (const auto& agent_id : thread->agent_ids) {
        auto agent = agents_.get_agent(agent_id);
        if (!agent) continue;
        fan_in(thread_id, *agent, "You", prompt_body, /*generation=*/0);
    }
    return msg_id;
}

std::string ChatService::post_nudge(const std::string& thread_id,
                                    const std::string& body) {
    if (stopping_)
        throw std::invalid_argument("server is shutting down");
    auto thread = threads_.get_thread(thread_id);
    if (!thread)
        throw std::invalid_argument("thread not found: " + thread_id);

    std::string msg_id =
        threads_.add_message(thread_id, "system", "hivemind", body);

    for (const auto& agent_id : thread->agent_ids) {
        auto agent = agents_.get_agent(agent_id);
        if (!agent) continue;
        fan_in(thread_id, *agent, "HiveMind", body, /*generation=*/0);
    }
    return msg_id;
}

// -- HubRouter callbacks -------------------------------------------------

void ChatService::append_delta(const std::string& task_id,
                               const std::string& text) {
    std::lock_guard<std::mutex> lk(m_);
    auto it = by_task_.find(task_id);
    if (it != by_task_.end()) it->second->partial += text;
}

void ChatService::complete_turn(const std::string& task_id,
                                const std::string& body,
                                const std::string& error,
                                const std::string& attachments_json) {
    std::shared_ptr<LiveTurn> live;
    {
        std::lock_guard<std::mutex> lk(m_);
        auto it = by_task_.find(task_id);
        if (it == by_task_.end()) return;      // unknown/cancelled turn
        live = it->second;
        by_task_.erase(it);
    }
    const std::string thread_id  = live->thread_id;
    const std::string agent_id   = live->agent_id;
    const std::string agent_name = live->agent_name;
    const int generation         = live->generation;

    // Exec audit: turns are the unit of agent activity in the worker
    // architecture (NDJSON stdio turns replaced per-command container
    // execs, which starved this table — System page flatlined 2026-08-29).
    // Log every completed turn so execs_24h / avg_ms / last_exec move.
    {
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - live->started).count();
        const bool to = error.find("timed out") != std::string::npos ||
                        error.find("timeout")   != std::string::npos;
        try {
            ContainerManager::exec_audit().log(agent_id, "<agent turn>",
                                               error.empty() ? 0 : 1, to, ms,
                                               live->worker);
        } catch (...) {}
    }

    std::string out = body;
    if (!error.empty()) {
        live->error = error;
        if (!out.empty())
            out += "\n\n*(turn ended with errors)*";
    }

    // Post only substantive output; silent context-only turns (the common
    // agent->agent fan-in terminator) don't flood the thread. Phase 35:
    // a turn whose only output is an ATTACHMENT still posts — otherwise
    // the file the agent attached would silently vanish.
    const bool has_atts = !attachments_json.empty() &&
                          attachments_json != "[]";
    if (out.empty() && has_atts) out = "*(attached a file)*";
    if ((!out.empty() && out != "(agent produced no output)") || has_atts) {
        bool posted = false;
        for (int attempt = 0; attempt < 2 && !posted; ++attempt) {
            try {
                threads_.add_message(thread_id, "agent", agent_id, out,
                                     attachments_json);
                posted = true;
            } catch (const std::exception& e) {
                std::fprintf(stderr,
                    "[chat] ERROR posting reply to thread %.8s "
                    "(attempt %d): %s\n",
                    thread_id.c_str(), attempt + 1, e.what());
            }
        }
        if (!posted)
            live->error = "reply generated but could not be posted";
        else if (generation < kMaxFanInGenerations) {
            try {
                auto thread = threads_.get_thread(thread_id);
                if (thread) {
                    for (const auto& peer_id : thread->agent_ids) {
                        if (peer_id == agent_id) continue;
                        auto peer = agents_.get_agent(peer_id);
                        if (peer)
                            fan_in(thread_id, *peer, agent_name, out,
                                   generation + 1);
                    }
                }
            } catch (...) { /* thread deleted mid-turn */ }
        }
    }
    live->done = true;
}

// -- polling / control ---------------------------------------------------

std::vector<ChatService::LiveTurn>
ChatService::activity(const std::string& thread_id) {
    std::lock_guard<std::mutex> lk(m_);
    std::vector<LiveTurn> out;
    auto it = live_.find(thread_id);
    if (it != live_.end())
        for (const auto& t : it->second) out.push_back(*t);
    return out;
}

bool ChatService::busy(const std::string& thread_id) {
    std::lock_guard<std::mutex> lk(m_);
    auto it = live_.find(thread_id);
    if (it == live_.end()) return false;
    for (const auto& t : it->second)
        if (!t->done) return true;
    return false;
}

std::unordered_map<std::string, ChatService::LiveTurn>
ChatService::agent_activity() {
    std::lock_guard<std::mutex> lk(m_);
    std::unordered_map<std::string, LiveTurn> out;
    for (const auto& kv : live_)
        for (const auto& t : kv.second)
            if (!t->done) out[t->agent_id] = *t;
    return out;
}

void ChatService::quiesce() { stopping_ = true; }

void ChatService::cancel_thread(const std::string& thread_id) {
    std::vector<RosterAgent> to_cancel;
    {
        std::lock_guard<std::mutex> lk(m_);
        auto it = live_.find(thread_id);
        if (it == live_.end()) return;
        for (auto& t : it->second) {
            if (t->done || t->cancelling) continue;
            t->cancelling = true;
            auto agent = agents_.get_agent(t->agent_id);
            if (agent) to_cancel.push_back(*agent);
        }
    }
    for (const auto& a : to_cancel) hub_.cancel_agent(a, thread_id);
}
