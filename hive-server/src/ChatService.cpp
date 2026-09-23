// ChatService (HiveMind 2.0 Phase C): hub-side thread<->agent glue.
// Turns run on workers; this class posts messages, dispatches tasks via
// HubRouter, and completes live turns when task_result frames arrive.

#include "hivemind/ChatService.h"
#include "hivemind/HubRouter.h"
#include "hivemind/AgentStore.h"
#include "hivemind/ThreadStore.h"
#include "hivemind/ContainerManager.h"
#include "hivemind/ExecAuditStore.h"
#include "hivemind/FanInPolicy.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
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
    record_relay(sender_id, tid);   // user-behalf pair msgs relay back
    fan_in(tid, *target, sender->name, body, /*generation=*/1);
    return tid;
}

std::chrono::seconds ChatService::relay_ttl() {
    const char* e = std::getenv("HIVEMIND_RELAY_TTL_S");
    const int v = e ? std::atoi(e) : 900;
    return std::chrono::seconds(v > 0 ? v : 900);
}

void ChatService::record_relay(const std::string& sender_id,
                               const std::string& pair_tid) {
    // Which thread is the sender working in RIGHT NOW? message_agent
    // arrives mid-turn, so the sender has a live turn in by_task_.
    std::string turn_thread;
    {
        std::lock_guard<std::mutex> lk(m_);
        for (const auto& [_, t] : by_task_) {
            if (t->agent_id == sender_id && !t->done && !t->cancelling) {
                turn_thread = t->thread_id;
                break;
            }
        }
    }
    if (turn_thread.empty()) return;
    // Lock released before ThreadStore (Phase 52 lock ordering).
    auto thread = threads_.get_thread(turn_thread);
    // Only user<->agent DM threads (kind "dm", exactly one agent) get a
    // relay — pair/group-thread agent chatter must never mirror into DMs.
    if (!thread || thread->kind != "dm" || thread->agent_ids.size() != 1)
        return;
    std::lock_guard<std::mutex> lk(m_);
    // Latest request wins: drop any stale link for this pair thread.
    std::erase_if(relay_links_, [&](const RelayLink& l) {
        return l.pair_thread_id == pair_tid; });
    relay_links_.push_back({pair_tid, sender_id, turn_thread,
                            std::chrono::steady_clock::now(), false});
    std::fprintf(stderr,
        "[chat] relay armed: pair %.8s -> user dm %.8s (requester %.8s)\n",
        pair_tid.c_str(), turn_thread.c_str(), sender_id.c_str());
}

void ChatService::maybe_fire_relay(const std::string& thread_id,
                                   const std::string& replier_id,
                                   const std::string& replier_name,
                                   const std::string& body,
                                   int generation) {
    RelayLink hit;
    // TEMP debug (2026-09-23 probe): why did the 07:00Z relay not fire?
    {
        std::lock_guard<std::mutex> lk(m_);
        std::fprintf(stderr,
            "[chat] relay probe: thread %.8s replier %.8s links=%zu\n",
            thread_id.c_str(), replier_id.c_str(), relay_links_.size());
        for (const auto& l : relay_links_)
            std::fprintf(stderr,
                "[chat] relay probe link: pair %.8s req %.8s dm %.8s consumed=%d\n",
                l.pair_thread_id.c_str(), l.requester_id.c_str(),
                l.user_thread_id.c_str(), (int)l.consumed);
    }
    {
        std::lock_guard<std::mutex> lk(m_);
        const auto now = std::chrono::steady_clock::now();
        std::erase_if(relay_links_, [&](const RelayLink& l) {
            return l.consumed || now - l.created > relay_ttl(); });
        for (auto& l : relay_links_) {
            if (l.pair_thread_id == thread_id && l.requester_id != replier_id) {
                l.consumed = true;   // one-shot
                hit = l;
                break;
            }
        }
    }
    if (hit.pair_thread_id.empty()) return;
    // Lock released before ThreadStore/fan_in (Phase 52 lock ordering).
    try {
        if (!threads_.get_thread(hit.user_thread_id)) return;
        auto requester = agents_.get_agent(hit.requester_id);
        if (!requester) return;
        // Mirror the peer's reply into the user DM (attributed to the
        // peer — every client just sees a new message), then wake the
        // requester there so it frames the answer for the user.
        threads_.add_message(hit.user_thread_id, "agent", replier_id, body);
        std::fprintf(stderr,
            "[chat] relay fired: %.8s's reply -> user dm %.8s\n",
            replier_id.c_str(), hit.user_thread_id.c_str());
        fan_in(hit.user_thread_id, *requester, replier_name, body,
               generation + 1);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "[chat] relay ERROR: %s\n", e.what());
    }
}

std::string ChatService::post_user_inbox_message(const std::string& sender_id,
                                                 const std::string& body) {
    if (stopping_)
        throw std::invalid_argument("server is shutting down");
    auto sender = agents_.get_agent(sender_id);
    if (!sender)
        throw std::invalid_argument("unknown sender agent");
    threads_.ensure_user_thread();
    threads_.add_message(kUserThreadId, "agent", sender_id, body);
    return kUserThreadId;
}

void ChatService::fan_in(const std::string& thread_id,
                         const RosterAgent& agent,
                         const std::string& sender_label,
                         const std::string& body,
                         int generation) {
    if (stopping_) return;

    // Phase 65a/65b: agent-triggered fan-in (generation > 0) respects
    // mutes and the circuit breaker. Generation-0 (user/nudge) fan-in is
    // never gated — post_user_message already lifted both.
    if (generation > 0) {
        bool just_tripped = false;
        bool allowed = true;
        {
            std::lock_guard<std::mutex> lk(m_);
            if (is_muted_locked(thread_id, agent.id)) {
                std::fprintf(stderr,
                    "[chat] fan-in dropped (muted): thread %.8s agent %.8s\n",
                    thread_id.c_str(), agent.id.c_str());
                return;
            }
            allowed = circuit_allow_locked(thread_id, &just_tripped);
        }
        // Lock released before posting (Phase 52 lock ordering:
        // m_ never held across ThreadStore calls).
        if (!allowed) {
            if (just_tripped) {
                try {
                    threads_.add_message(thread_id, "system", "hivemind",
                        "*(circuit breaker tripped — too many agent turns "
                        "in this thread; agent fan-in paused. Send a "
                        "message to resume.)*");
                } catch (...) {}
            }
            return;
        }
    }

    // The agent's hive-brain conversation (persisted on the worker's
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
            // Visibility: without this the user message sits at
            // "processing" forever with no explanation (hit 2026-09-07
            // when a dispatch raced the worker's hub reconnect).
            try {
                threads_.add_message(thread_id, "system", "hivemind",
                    "*(dispatch to agent " + agent.name +
                    " failed — worker offline or no LLM config; "
                    "resend the message to retry)*");
            } catch (...) {}
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

    // Phase 65a/65b: a user message is the explicit "speak now" signal —
    // lift the thread mute, every participant's agent mute, and reset the
    // circuit breaker BEFORE the generation-0 fan-in below.
    {
        std::lock_guard<std::mutex> lk(m_);
        unmute_thread_locked(thread_id, thread->agent_ids);
    }

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
                                const std::string& attachments_json,
                                const std::string& frame_thread_id,
                                const std::string& frame_agent_id,
                                const std::string& stats_json) {
    std::shared_ptr<LiveTurn> live;
    {
        std::lock_guard<std::mutex> lk(m_);
        auto it = by_task_.find(task_id);
        if (it != by_task_.end()) {
            live = it->second;
            by_task_.erase(it);
        }
    }
    if (!live) {
        // Watchdog/manual-cancel tombstone: a late result for a cancelled
        // task is dropped silently, NOT treated as a restart orphan.
        {
            std::lock_guard<std::mutex> lk(m_);
            if (cancelled_tasks_.erase(task_id) > 0) return;
        }
        // Phase 39 H2 — restart recovery. A hub restart wipes in-memory
        // LiveTurns but workers keep running in-flight turns; when the
        // result arrives the task is unknown. The frame still carries
        // thread_id/agent_id (hive-agent main.cpp:517 → worker
        // main.cpp:142), so post the reply directly. Terminal: no fan-in
        // continuation — generation state died with the restart.
        if (frame_thread_id.empty() || frame_agent_id.empty()) return;
        if (body.empty() && error.empty()) return;
        try {
            if (!threads_.get_thread(frame_thread_id)) return;
            std::string out = body;
            if (!error.empty()) out += "\n\n*(turn ended with errors)*";
            out += "\n\n*(recovered after hub restart)*";
            threads_.add_message(frame_thread_id, "agent", frame_agent_id,
                                 out, attachments_json, stats_json);
            std::fprintf(stderr,
                "[chat] recovered orphaned turn %.16s → thread %.8s "
                "(hub restart)\n", task_id.c_str(), frame_thread_id.c_str());
        } catch (const std::exception& e) {
            std::fprintf(stderr, "[chat] ERROR recovering orphaned turn: %s\n",
                         e.what());
        }
        return;
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
                                     attachments_json, stats_json);
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
        else {
            // Relay fires on ANY posted reply — the 2026-09-23 probe
            // proved the bug: Honey's "RELAY-PROBE-OK … acknowledged"
            // reply was ack-only, and with the hook behind the ack guard
            // the answer never reached the user's DM. An ack-looking peer
            // reply IS the answer the user is waiting for; ack-only must
            // gate ONLY the fan-in continuation below (65c).
            maybe_fire_relay(thread_id, agent_id, agent_name, out,
                             generation);
            if (generation < kMaxFanInGenerations &&
                !hivemind::fanin_is_ack_only(out)) {   // 65c: acks post
                                                       // but never fan
            try {
                auto thread = threads_.get_thread(thread_id);
                if (thread) {
                    if (thread->agent_ids.size() > 2) {
                        // 65h: >2-agent threads — fan in ONLY to peers
                        // explicitly addressed (@Name / [to:Name]).
                        // Undirected replies are visible in the thread
                        // but task nobody (storm fix 2026-09-16: N-1
                        // branching made broadcast exponential).
                        std::vector<std::pair<std::string, std::string>>
                            candidates;
                        for (const auto& peer_id : thread->agent_ids) {
                            if (peer_id == agent_id) continue;
                            auto peer = agents_.get_agent(peer_id);
                            if (peer)
                                candidates.emplace_back(peer->id,
                                                        peer->name);
                        }
                        for (const auto& pid :
                                 hivemind::fanin_mentioned_ids(out,
                                                               candidates)) {
                            auto peer = agents_.get_agent(pid);
                            if (peer)
                                fan_in(thread_id, *peer, agent_name, out,
                                       generation + 1);
                        }
                    } else {
                        // 2-agent threads: broadcast is self-damping
                        // (branching factor 1) — keep it.
                        for (const auto& peer_id : thread->agent_ids) {
                            if (peer_id == agent_id) continue;
                            auto peer = agents_.get_agent(peer_id);
                            if (peer)
                                fan_in(thread_id, *peer, agent_name, out,
                                       generation + 1);
                        }
                    }
                }
            } catch (...) { /* thread deleted mid-turn */ }
            }
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

// ── Phase 39 H3: turn watchdog ───────────────────────────────────────

int ChatService::turn_timeout_s() {
    static const int v = [] {
        const char* e = std::getenv("HIVEMIND_TURN_TIMEOUT_S");
        if (!e || !*e) return 1200;
        const int n = std::atoi(e);
        return n >= 0 ? n : 1200;
    }();
    return v;
}

int ChatService::cancel_grace_s() {
    static const int v = [] {
        const char* e = std::getenv("HIVEMIND_CANCEL_GRACE_S");
        if (!e || !*e) return 90;
        const int n = std::atoi(e);
        return n > 0 ? n : 90;
    }();
    return v;
}

void ChatService::start_watchdog() {
    if (turn_timeout_s() <= 0) return;   // disabled
    watchdog_ = std::thread([this] { watchdog_loop(); });
}

ChatService::~ChatService() {
    stopping_ = true;
    if (watchdog_.joinable()) watchdog_.join();
}

void ChatService::watchdog_loop() {
    while (!stopping_) {
        // 60s tick, but wake promptly on shutdown.
        for (int i = 0; i < 60 && !stopping_; ++i)
            std::this_thread::sleep_for(std::chrono::seconds(1));
        if (stopping_) break;

        const int timeout = turn_timeout_s();
        struct Victim { std::string task_id, thread_id, agent_id;
                        bool was_cancelling = false;
                        std::shared_ptr<LiveTurn> turn; };
        std::vector<Victim> victims;
        {
            std::lock_guard<std::mutex> lk(m_);
            const auto now = std::chrono::steady_clock::now();
            for (auto& [tid, vec] : live_) {
                for (auto& t : vec) {
                    if (t->done || t->task_id.empty())
                        continue;
                    const auto now_s = now;
                    const bool already_cancelling = t->cancelling;
                    if (t->cancelling) {
                        // Cancel never confirmed by the worker (dropped task
                        // frame, dead worker, idle brain) — retire after the
                        // grace period or the turn wedges busy forever.
                        if (t->cancel_started ==
                            std::chrono::steady_clock::time_point{})
                            t->cancel_started = now_s;
                        const auto cs = std::chrono::duration_cast<
                            std::chrono::seconds>(now_s - t->cancel_started).count();
                        if (cs < cancel_grace_s()) continue;
                    } else {
                        const auto age = std::chrono::duration_cast<
                            std::chrono::seconds>(now_s - t->started).count();
                        if (age < timeout) continue;
                        t->cancelling = true;
                        t->cancel_started = now_s;
                        if (!t->task_id.empty())
                            cancelled_tasks_.insert(t->task_id);
                    }
                    victims.push_back({t->task_id, t->thread_id, t->agent_id,
                                       already_cancelling, t});
                }
            }
        }
        for (const auto& v : victims) {
            std::fprintf(stderr,
                "[chat] WATCHDOG: turn %.16s on agent %.8s %s — retiring\n",
                v.task_id.c_str(), v.agent_id.c_str(),
                v.was_cancelling
                    ? "cancel unconfirmed by worker (grace elapsed)"
                    : ("exceeded " + std::to_string(timeout) + "s").c_str());
            // Abort worker-side so the agent's serial queue unblocks.
            if (auto agent = agents_.get_agent(v.agent_id)) {
                try { hub_.cancel_agent(*agent, v.thread_id); }
                catch (...) {}
            }
            // Phase 52: preserve whatever the agent said before dying.
            // (Cancel-path turns were already flushed by cancel_thread;
            // flush_partial is idempotent via the flag.)
            if (v.turn) flush_partial(*v.turn);
            // Tell the thread what happened, then retire the turn.
            try {
                threads_.add_message(v.thread_id, "system", "hivemind",
                    v.was_cancelling
                        ? "*(the cancel was never confirmed by the worker — "
                          "turn retired by the hub watchdog)*"
                        : "*(a turn timed out after " + std::to_string(timeout) +
                          "s and was cancelled — hub watchdog)*");
            } catch (...) {}
            std::lock_guard<std::mutex> lk(m_);
            auto it = live_.find(v.thread_id);
            if (it != live_.end())
                for (auto& t : it->second)
                    if (t->task_id == v.task_id) t->done = true;
            by_task_.erase(v.task_id);
        }
    }
}

void ChatService::flush_partial(LiveTurn& t) {
    if (t.partial_flushed || t.partial.empty()) return;
    t.partial_flushed = true;
    // The turn's own final result will NOT arrive (cancelled turns are
    // tombstoned in complete_turn), so this is the only place the
    // user-facing commentary survives. Posted as the agent with an
    // interrupted marker.
    try {
        threads_.add_message(t.thread_id, "agent", t.agent_id,
                             t.partial + "\n\n*(interrupted)*");
    } catch (const std::exception& e) {
        std::fprintf(stderr, "[chat] ERROR flushing partial for %.16s: %s\n",
                     t.task_id.c_str(), e.what());
    }
}

int ChatService::cancel_agent_turns(const std::string& agent_id) {
    std::vector<std::pair<RosterAgent, std::string>> to_cancel;
    std::vector<std::shared_ptr<LiveTurn>> to_flush;
    int count = 0;
    {
        std::lock_guard<std::mutex> lk(m_);
        for (auto& [thread_id, turns] : live_) {
            for (auto& t : turns) {
                if (t->agent_id != agent_id || t->done || t->cancelling)
                    continue;
                t->cancelling = true;
                t->cancel_started = std::chrono::steady_clock::now();
                if (!t->task_id.empty()) cancelled_tasks_.insert(t->task_id);
                auto agent = agents_.get_agent(t->agent_id);
                if (agent) to_cancel.emplace_back(*agent, thread_id);
                to_flush.push_back(t);   // Phase 52: keep the commentary
                ++count;
            }
        }
    }
    // 65a: Stop = MUTE. Even when count == 0 (agent idle) the user's
    // intent is "do not let fan-in start new work for this agent".
    {
        std::lock_guard<std::mutex> lk(m_);
        mute_agent_locked(agent_id);
    }
    // Same ordering as cancel_thread: cancels out of the lock, then
    // flush so the interrupt note lands after any in-flight text.
    for (const auto& [a, tid] : to_cancel) hub_.cancel_agent(a, tid);
    for (auto& t : to_flush) flush_partial(*t);
    return count;
}

void ChatService::cancel_thread(const std::string& thread_id) {
    std::vector<RosterAgent> to_cancel;
    std::vector<std::shared_ptr<LiveTurn>> to_flush;
    {
        std::lock_guard<std::mutex> lk(m_);
        auto it = live_.find(thread_id);
        if (it == live_.end()) return;
        for (auto& t : it->second) {
            if (t->done || t->cancelling) continue;
            t->cancelling = true;
            t->cancel_started = std::chrono::steady_clock::now();
            if (!t->task_id.empty()) cancelled_tasks_.insert(t->task_id);
            auto agent = agents_.get_agent(t->agent_id);
            if (agent) to_cancel.push_back(*agent);
            to_flush.push_back(t);   // Phase 52: keep the commentary
        }
    }
    // 65a: Stop = MUTE for the whole thread.
    {
        std::lock_guard<std::mutex> lk(m_);
        mute_thread_locked(thread_id);
    }
    for (const auto& a : to_cancel) hub_.cancel_agent(a, thread_id);
    // Phase 52: flush AFTER the lock (ThreadStore lock ordering) and
    // after cancel_agent so the interrupt note lands after any in-flight
    // text. Late results are tombstoned — this is the surviving record.
    for (auto& t : to_flush) flush_partial(*t);
}

// -- Phase 65a/65b helpers ------------------------------------------------

int ChatService::mute_s() {
    const char* e = std::getenv("HIVEMIND_MUTE_S");
    if (e && *e) { try { return std::max(0, std::stoi(e)); } catch (...) {} }
    return 600;   // 10 min default
}

int ChatService::cb_max_turns() {
    const char* e = std::getenv("HIVEMIND_CB_MAX_TURNS");
    if (e && *e) { try { return std::max(1, std::stoi(e)); } catch (...) {} }
    return 20;
}

int ChatService::cb_window_s() {
    const char* e = std::getenv("HIVEMIND_CB_WINDOW_S");
    if (e && *e) { try { return std::max(1, std::stoi(e)); } catch (...) {} }
    return 300;
}

bool ChatService::is_muted_locked(const std::string& thread_id,
                                  const std::string& agent_id) {
    const auto now = std::chrono::steady_clock::now();
    auto check = [&](auto& map, const std::string& key) {
        auto it = map.find(key);
        if (it == map.end()) return false;
        if (now >= it->second) { map.erase(it); return false; }  // expired
        return true;
    };
    return check(muted_threads_, thread_id) ||
           check(muted_agents_, agent_id);
}

void ChatService::mute_agent_locked(const std::string& agent_id) {
    muted_agents_[agent_id] =
        std::chrono::steady_clock::now() + std::chrono::seconds(mute_s());
}

void ChatService::mute_thread_locked(const std::string& thread_id) {
    muted_threads_[thread_id] =
        std::chrono::steady_clock::now() + std::chrono::seconds(mute_s());
}

void ChatService::unmute_thread_locked(
        const std::string& thread_id,
        const std::vector<std::string>& agent_ids) {
    muted_threads_.erase(thread_id);
    throttle_.erase(thread_id);   // breaker resets too
    for (const auto& id : agent_ids)
        muted_agents_.erase(id);
}

bool ChatService::circuit_allow_locked(const std::string& thread_id,
                                       bool* just_tripped) {
    *just_tripped = false;
    auto& th = throttle_[thread_id];
    if (th.tripped) return false;
    const auto now = std::chrono::steady_clock::now();
    const auto cutoff = now - std::chrono::seconds(cb_window_s());
    while (!th.fanins.empty() && th.fanins.front() < cutoff)
        th.fanins.pop_front();
    th.fanins.push_back(now);
    if (static_cast<int>(th.fanins.size()) > cb_max_turns()) {
        th.tripped = true;
        if (!th.announced) { th.announced = true; *just_tripped = true; }
        std::fprintf(stderr,
            "[chat] circuit breaker TRIPPED for thread %.8s (%d fan-ins in "
            "%ds)\n", thread_id.c_str(), cb_max_turns(), cb_window_s());
        return false;
    }
    return true;
}
