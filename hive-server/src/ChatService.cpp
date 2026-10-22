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
#include "hivemind/DeliveryStore.h"
#include "hivemind/MessageHeader.h"
#include "hivemind/DbUtil.h"

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
                                            const std::string& body,
                                            std::string* delivery_id_out) {
    if (delivery_id_out) delivery_id_out->clear();
    if (stopping_)
        throw std::invalid_argument("server is shutting down");
    auto sender = agents_.get_agent(sender_id);
    auto target = agents_.get_agent(target_id);
    if (!sender || !target)
        throw std::invalid_argument("unknown sender or target agent");

    // Dedup (2026-09-25, Chief's double-delivery report): an identical
    // (sender, target, body) inside the window is the sender retrying a
    // slow tool call, not a new message — drop the repeat so retries
    // can't double-post. Returns "" (caller ignores the thread id).
    {
        std::lock_guard<std::mutex> lk(m_);
        const auto now = std::chrono::steady_clock::now();
        const auto win = std::chrono::seconds(dedup_window_s());
        while (!recent_agent_msgs_.empty() &&
               now - recent_agent_msgs_.front().at > win)
            recent_agent_msgs_.pop_front();
        for (const auto& r : recent_agent_msgs_) {
            if (r.sender == sender_id && r.target == target_id &&
                r.body == body) {
                std::fprintf(stderr,
                    "[chat] agent_msg dedup: %.8s -> %.8s identical body "
                    "within %ds — repeat dropped\n",
                    sender_id.c_str(), target_id.c_str(),
                    dedup_window_s());
                return "";
            }
        }
        recent_agent_msgs_.push_back({sender_id, target_id, body, now});
    }

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

    const std::string msg_id =
        threads_.add_message(tid, "agent", sender_id, body);
    record_relay(sender_id, tid);   // user-behalf pair msgs relay back
    // Read receipts (Phase 66c hook): sending implies read-to-own-message.
    if (auto m = threads_.get_message(msg_id))
        deliveries_.mark_read(tid, sender_id, msg_id, m->created_at);
    const std::string did =
        fan_in(tid, *target, sender->name,
               std::string(hivemind::msg_source::kAgentPrefix) +
                   sender->name,
               body, /*generation=*/1, sender_id, msg_id, "wake");
    if (delivery_id_out) *delivery_id_out = did;
    return tid;
}

std::vector<hivemind::Delivery>
ChatService::delivery_status(const std::string& agent_id,
                             const std::string& delivery_id) {
    if (!delivery_id.empty()) {
        auto d = deliveries_.get(delivery_id);
        // Only the sender or the target may see a delivery.
        if (!d || (d->sender_agent_id != agent_id &&
                   d->target_agent_id != agent_id))
            return {};
        return {*d};
    }
    return deliveries_.sent_by(agent_id, 10);
}

std::vector<hivemind::Delivery>
ChatService::deliveries_list(const std::string& agent_id,
                             const std::string& state, int limit) {
    return deliveries_.list(agent_id, state, limit);
}

bool ChatService::delivery_is_read(const hivemind::Delivery& d) {
    if (d.source_message_id.empty()) return false;
    const auto mark = deliveries_.read_mark(d.thread_id, d.target_agent_id);
    if (!mark) return false;
    const auto msg = threads_.get_message(d.source_message_id);
    if (!msg) return false;
    return *mark >= msg->created_at;
}

void ChatService::user_mark_read(const std::string& thread_id) {
    const auto msgs = threads_.list_messages(thread_id);
    if (msgs.empty()) return;
    const auto& last = msgs.back();
    deliveries_.mark_read(thread_id, "user", last.id, last.created_at);
}

int ChatService::unread_for_user(const std::string& thread_id) {
    return threads_.count_non_user_since(
        thread_id, deliveries_.read_mark(thread_id, "user").value_or(""));
}

std::optional<std::string> ChatService::read_mark(
    const std::string& thread_id, const std::string& agent_id) {
    return deliveries_.read_mark(thread_id, agent_id);
}

std::chrono::seconds ChatService::relay_ttl() {
    const char* e = std::getenv("HIVEMIND_RELAY_TTL_S");
    const int v = e ? std::atoi(e) : 900;
    return std::chrono::seconds(v > 0 ? v : 900);
}

int ChatService::wake_ttl_s() {
    static const int v = [] {
        const char* e = std::getenv("HIVEMIND_WAKE_TTL_S");
        if (!e || !*e) return 86400;
        const int n = std::atoi(e);
        return n > 0 ? n : 86400;
    }();
    return v;
}

int ChatService::dedup_window_s() {
    static const int v = [] {
        const char* e = std::getenv("HIVEMIND_MSG_DEDUP_S");
        if (!e || !*e) return 30;
        const int n = std::atoi(e);
        return n >= 0 ? n : 30;
    }();
    return v;
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
    if (turn_thread == pair_tid) return;   // home must differ from the
                                           // pair thread (a "relay" into
                                           // the same thread would
                                           // double-post the reply)
    // Phase 66b: ANY thread kind is a valid relay home — the old
    // dm-only gate is why Chief's 21:00 ROUTINE check-ins never armed a
    // relay (2026-09-26 blind spot). Durable: the link survives a hub
    // restart (DeliveryStore), latest arm wins per pair thread.
    deliveries_.arm_relay(pair_tid, sender_id, turn_thread);
    std::fprintf(stderr,
        "[delivery] relay armed: pair %.8s -> home %.8s (requester %.8s)\n",
        pair_tid.c_str(), turn_thread.c_str(), sender_id.c_str());
}

std::string ChatService::maybe_fire_relay(const std::string& thread_id,
                                          const std::string& replier_id,
                                          const std::string& replier_name,
                                          const std::string& body,
                                          int generation) {
    // Durable one-shot (Phase 66b): consume_relay returns the link only
    // when one is armed AND the replier is not the requester.
    const auto hit = deliveries_.consume_relay(thread_id, replier_id);
    if (!hit) return "";
    try {
        if (!threads_.get_thread(hit->home_thread_id)) return "";
        auto requester = agents_.get_agent(hit->requester_id);
        if (!requester) return "";
        // Mirror the peer's reply into the requester's home thread
        // (attributed to the peer), then wake the requester there to
        // frame it. A BUSY requester just keeps the relay wake 'queued'
        // — the durable queue drains on idle; no hold-queue needed.
        const std::string mirror_id = threads_.add_message(
            hit->home_thread_id, "agent", replier_id, body);
        std::fprintf(stderr,
            "[delivery] relay fired: %.8s's reply -> home %.8s "
            "(requester %.8s)\n",
            replier_id.c_str(), hit->home_thread_id.c_str(),
            hit->requester_id.c_str());
        fan_in(hit->home_thread_id, *requester, replier_name,
               std::string(hivemind::msg_source::kAgentPrefix) +
                   replier_name,
               body, generation + 1, replier_id, mirror_id, "relay");
        return hit->requester_id;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "[delivery] relay ERROR: %s\n", e.what());
        return "";
    }
}

bool ChatService::agent_busy(const std::string& agent_id) {
    std::lock_guard<std::mutex> lk(m_);
    return agent_busy_locked(agent_id);
}

bool ChatService::agent_busy_locked(const std::string& agent_id) const {
    for (const auto& [_, turns] : live_)
        for (const auto& t : turns)
            if (t->agent_id == agent_id && !t->done && !t->cancelling)
                return true;
    return false;
}

void ChatService::on_agent_ready(const std::string& agent_id) {
    // Agent reported "ready" (spawn/reconnect): drain its durable queue.
    try_dispatch(agent_id);
}

std::string ChatService::post_user_inbox_message(const std::string& sender_id,
                                                 const std::string& body) {
    if (stopping_)
        throw std::invalid_argument("server is shutting down");
    auto sender = agents_.get_agent(sender_id);
    if (!sender)
        throw std::invalid_argument("unknown sender agent");
    threads_.ensure_user_thread();
    const std::string msg_id =
        threads_.add_message(kUserThreadId, "agent", sender_id, body);
    // Read receipts: sending implies read-to-own-message.
    if (auto m = threads_.get_message(msg_id))
        deliveries_.mark_read(kUserThreadId, sender_id, msg_id,
                              m->created_at);
    return kUserThreadId;
}

std::string ChatService::fan_in(const std::string& thread_id,
                         const RosterAgent& agent,
                         const std::string& sender_label,
                         const std::string& source,
                         const std::string& body,
                         int generation,
                         const std::string& sender_id,
                         const std::string& source_message_id,
                         const std::string& kind) {
    if (stopping_) return "";

    // Idempotency (Phase 66b): a queued/dispatched wake for the same
    // (target, source message) suppresses the duplicate — replaces the
    // content-window dedup on the wake path.
    if (deliveries_.has_live_wake(agent.id, source_message_id)) {
        std::fprintf(stderr,
            "[delivery] wake suppressed (dup): -> %.8s msg %.8s\n",
            agent.id.c_str(), source_message_id.c_str());
        return "";
    }

    hivemind::Delivery d;
    d.kind              = kind;
    d.thread_id         = thread_id;
    d.target_agent_id   = agent.id;
    d.sender_agent_id   = sender_id;
    d.sender_label      = sender_label;
    d.source            = source;
    d.body              = body;
    d.generation        = generation;
    d.source_message_id = source_message_id;
    const std::string id = deliveries_.enqueue(std::move(d));
    std::fprintf(stderr,
        "[delivery] queued %.8s (%s): -> %.8s thread %.8s gen %d\n",
        id.c_str(), kind.c_str(), agent.id.c_str(), thread_id.c_str(),
        generation);
    try_dispatch(agent.id);
    return id;
}

namespace {
/// §MsgStatus (2026-10-22): agent-facing id+marker line appended to
/// every wake segment. The ids make the segment addressable for the
/// message_mark tool (agents see wakes, not raw thread JSON, so this
/// is the ONLY place they learn message ids); the MARKED clause is
/// rendered from the LIVE message row at dispatch time — a claim/done
/// set after enqueue still shows, which is the whole point (stops
/// redelivery and parallel-session re-runs; QueenBee bug classes 1+4).
std::string wake_status_line(ThreadStore& threads,
                             const std::string& thread_id,
                             const std::string& source_message_id) {
    if (source_message_id.empty()) return {};
    const auto m = threads.get_message(source_message_id);
    if (!m) return {};
    std::string out = "(thread " + thread_id + ", msg " + m->id;
    if (m->status.empty())
        return out + ") ";
    out += ") MARKED " + m->status + " by " + m->status_by + " @ " +
           m->status_at;
    if (!m->status_note.empty()) out += " — \"" + m->status_note + "\"";
    return out + " — do NOT redo this work. ";
}
}  // namespace

void ChatService::try_dispatch(const std::string& agent_id) {
    if (stopping_) return;
    for (;;) {
        {
            std::lock_guard<std::mutex> lk(m_);
            if (agent_busy_locked(agent_id)) return;
        }
        // Lock released before stores (Phase 52 ordering).
        const auto d = deliveries_.oldest_queued(agent_id);
        if (!d) return;
        auto agent = agents_.get_agent(agent_id);
        if (!agent) {
            deliveries_.mark_dropped(d->id, "agent deleted");
            std::fprintf(stderr,
                "[delivery] dropped %.8s: agent %.8s gone\n",
                d->id.c_str(), agent_id.c_str());
            continue;
        }
        if (!threads_.get_thread(d->thread_id)) {
            deliveries_.mark_dropped(d->id, "thread deleted");
            std::fprintf(stderr,
                "[delivery] dropped %.8s: thread %.8s gone\n",
                d->id.c_str(), d->thread_id.c_str());
            continue;
        }

        // Phase 65a/65b gates, checked AT DISPATCH TIME (a mute set
        // while the wake was queued still applies). Generation-0
        // (user/nudge) wakes are never gated. A refusal is RECORDED on
        // the row — no more silent gates.
        if (d->generation > 0) {
            bool muted = false, allowed = true, just_tripped = false;
            {
                std::lock_guard<std::mutex> lk(m_);
                muted = is_muted_locked(d->thread_id, agent_id);
                if (!muted)
                    allowed = circuit_allow_locked(d->thread_id,
                                                   &just_tripped);
            }
            if (muted) {
                deliveries_.mark_dropped(d->id, "muted");
                std::fprintf(stderr,
                    "[delivery] dropped %.8s (muted): -> %.8s thread %.8s\n",
                    d->id.c_str(), agent_id.c_str(), d->thread_id.c_str());
                continue;
            }
            if (!allowed) {
                deliveries_.mark_dropped(d->id, "circuit breaker");
                std::fprintf(stderr,
                    "[delivery] dropped %.8s (circuit breaker): -> %.8s "
                    "thread %.8s\n",
                    d->id.c_str(), agent_id.c_str(), d->thread_id.c_str());
                if (just_tripped) {
                    try {
                        threads_.add_message(d->thread_id, "system",
                            "hivemind",
                            "*(circuit breaker tripped — too many agent "
                            "turns in this thread; agent fan-in paused. "
                            "Send a message to resume.)*");
                    } catch (...) {}
                }
                continue;
            }
        }

        // The agent's hive-brain conversation (persisted on the worker's
        // /workspace volume) IS the context - each task carries ONLY the
        // new message. DO NOT add a thread-history backlog (v1 lesson).
        //
        // 2026-10-03 stray-fire fix (Plan.md): every wake carries a
        // header rendered from the STORED source + created_at — the
        // delivery row's created_at is stamped at ENQUEUE, so a wake
        // queued at 01:00 and dispatched at 09:00 (worker offline,
        // was-queued drain) still reads as the 01:00 message. The
        // brain keeps ONE shared, untimestamped conversation across
        // all of the agent's threads (HoneyBee's ~15 phantom routine
        // re-fires); an explicit UTC source+time stamp makes old
        // context self-evidently old and says WHICH channel it came
        // on. Voice wakes additionally carry the TTS channel hint
        // (moved here server-side from the Android OUTGOING_PREFIX).
        //
        // 2026-10-04 queue-join (Plan.md): when the sender bursts several
        // messages while the agent is busy, the wakes queue and used to
        // drain as N sequential turns — N LLM calls and the agent could
        // act on message 1 before seeing the correction in message 3.
        // Queued wakes in the SAME thread at the SAME fan-in depth now
        // JOIN into one prompt — for user bursts (gen 0) and agent
        // fan-in bursts (gen>0, lee 2026-10-04) alike: one task, one
        // turn, each segment keeping its own [source @ enqueue-time]
        // header. Every joined row is stamped with the SAME task_id, so
        // completion/watchdog/cancel paths transition all of them
        // (DeliveryStore::find_all_by_task). Generation-scoped because
        // complete_turn's fan-in continuation is depth+1. The mute/
        // circuit gates above already ran on the head row and the state
        // is per (thread, agent), so every same-thread join member
        // shares the verdict. The circuit breaker counts DISPATCHES
        // (turns), not messages — a joined batch is one turn, so an
        // A→B→A ping-pong still trips at the same turn rate.
        constexpr int kMaxJoin = 10;   // same cap as the phone fold
        std::vector<std::string> joined_ids{d->id};
        std::string prompt =
            hivemind::render_message_header(d->source, d->created_at) +
            wake_status_line(threads_, d->thread_id, d->source_message_id) +
            d->body;
        bool any_voice = (d->source == hivemind::msg_source::kVoiceApp);
        {
            auto join_set = deliveries_.queued_for_thread(
                agent_id, d->thread_id, d->generation, kMaxJoin);
            if (join_set.size() > 1) {
                prompt.clear();
                for (const auto& j : join_set) {
                    prompt += hivemind::render_message_header(
                                  j.source, j.created_at) +
                              wake_status_line(threads_, j.thread_id,
                                               j.source_message_id) +
                              j.body + "\n\n";
                    if (j.id != d->id) joined_ids.push_back(j.id);
                    any_voice = any_voice ||
                                (j.source == hivemind::msg_source::kVoiceApp);
                }
                // Drop the trailing blank line between segments.
                prompt.resize(prompt.size() >= 2 ? prompt.size() - 2 : 0);
            }
        }
        if (any_voice)
            prompt += "\n" + std::string(hivemind::kVoiceChannelHint);

        auto live = std::make_shared<LiveTurn>();
        live->thread_id  = d->thread_id;
        live->agent_id   = agent->id;
        live->agent_name = agent->name;
        live->worker     = agent->worker;
        live->generation = d->generation;

        const std::string task_id =
            hub_.dispatch(*agent, d->thread_id, prompt);
        if (task_id.empty()) {
            // Worker offline: leave the row QUEUED (durable — it drains
            // when the worker reconnects and the agent reports ready).
            // Visibility system message, same as the old inline path.
            std::fprintf(stderr,
                "[delivery] dispatch deferred %.8s: worker offline "
                "(-> %.8s)\n", d->id.c_str(), agent_id.c_str());
            {
                std::lock_guard<std::mutex> lk(m_);
                auto& vec = live_[d->thread_id];
                live->error = "worker offline";
                live->done = true;
                vec.push_back(live);
            }
            try {
                threads_.add_message(d->thread_id, "system", "hivemind",
                    "*(dispatch to agent " + agent->name +
                    " deferred — worker offline or no LLM config; the "
                    "message is queued and will be delivered when the "
                    "worker is back)*");
            } catch (...) {}
            return;   // everything for this agent will fail the same way
        }

        {
            std::lock_guard<std::mutex> lk(m_);
            auto& vec = live_[d->thread_id];
            vec.erase(std::remove_if(vec.begin(), vec.end(),
                                     [](const auto& t) { return t->done; }),
                      vec.end());
            if (vec.size() >= kMaxLivePerThread)
                vec.erase(vec.begin());
            live->task_id = task_id;
            vec.push_back(live);
            by_task_[task_id] = live;
        }
        // Queue-join (Plan.md, 2026-10-04): every joined row carries the
        // SAME task_id, so the turn's completion/watchdog/cancel paths
        // close them all via DeliveryStore::find_all_by_task.
        for (const auto& jid : joined_ids)
            deliveries_.mark_dispatched(jid, task_id);
        const std::string joined_note =
            joined_ids.size() > 1
                ? ", joined " + std::to_string(joined_ids.size()) +
                  " queued wakes"
                : "";
        std::fprintf(stderr,
            "[delivery] dispatched %.8s -> %.8s thread %.8s (task %.12s%s)\n",
            d->id.c_str(), agent_id.c_str(), d->thread_id.c_str(),
            task_id.c_str(), joined_note.c_str());
        return;   // ONE wake per idle transition
    }
}

std::string ChatService::post_user_message(const std::string& thread_id,
                                           const std::string& body,
                                           const std::string& attachments_json,
                                           const std::string& source) {
    if (stopping_)
        throw std::invalid_argument("server is shutting down");
    auto thread = threads_.get_thread(thread_id);
    if (!thread)
        throw std::invalid_argument("thread not found: " + thread_id);

    // ── Phone-notification fold (2026-10-01) ────────────────────
    // Notifications relayed by the phone app (POST /api/notifications)
    // sit in this thread as SILENT system messages — never dispatched
    // on their own. Fold the ones posted since the previous user
    // message into THIS wake's prompt, so the agent can answer "what
    // did I get?" without a turn per notification. Selection is pure
    // thread geometry (the trailing [phone · block after the last
    // user message) — no watermark state, restart-safe.
    std::string notify_block;
    {
        bool has_more = false;
        auto recent = threads_.list_messages_window(thread_id, "", 50, has_more);
        // Each folded row gets its OWN rendered header (phone source +
        // the row's STORED created_at) so old notifications are
        // visibly old in context — the fold's blanket "since we last
        // spoke" is not a timestamp (Plan.md §3.4). The in-body
        // "[phone · app]" tag stays: it is the fold's selection marker.
        std::vector<std::pair<std::string, std::string>> pending;
        for (auto it = recent.rbegin(); it != recent.rend(); ++it) {
            if (it->sender_kind == "user") break;
            if (it->sender_kind == "system" &&
                it->body.rfind("[phone ", 0) == 0)
                pending.emplace_back(it->created_at, it->body);
        }
        if (!pending.empty()) {
            constexpr size_t kMaxFold = 10;
            if (pending.size() > kMaxFold)
                pending.erase(pending.begin(),
                          pending.begin() + (pending.size() - kMaxFold));
            std::reverse(pending.begin(), pending.end());
            std::string joined;
            for (const auto& [at, body] : pending)
                joined += "  " +
                    hivemind::render_message_header(
                        hivemind::msg_source::kPhone, at) +
                    body + "\n";
            notify_block =
                "Phone notifications since we last spoke:\n" + joined + "\n";
            std::fprintf(stderr,
                "[notify] folded %zu phone notification(s) into wake, "
                "thread %.8s\n", pending.size(), thread_id.c_str());
        }
    }
    std::string msg_id =
        threads_.add_message(thread_id, "user", "", body, attachments_json);

    // Phase 65a/65b: a user message is the explicit "speak now" signal —
    // lift the thread mute, every participant's agent mute, and reset the
    // circuit breaker BEFORE the generation-0 fan-in below.
    {
        std::lock_guard<std::mutex> lk(m_);
        unmute_thread_locked(thread_id, thread->agent_ids);
    }

    const std::string prompt_body =
        (notify_block.empty() ? "" : notify_block) +
        body + attachment_refs(attachments_json);

    for (const auto& agent_id : thread->agent_ids) {
        auto agent = agents_.get_agent(agent_id);
        if (!agent) continue;
        fan_in(thread_id, *agent, "You", source, prompt_body,
               /*generation=*/0, "", msg_id, "wake");
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
        fan_in(thread_id, *agent, "HiveMind",
               std::string(hivemind::msg_source::kSystem), body,
               /*generation=*/0, "", msg_id, "wake");
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

void ChatService::post_turn_attachment(
        const std::string& task_id, const std::string& thread_id,
        const std::string& agent_id, const std::string& attachments_json) {
    if (attachments_json.empty() || attachments_json == "[]") return;
    if (thread_id.empty() || agent_id.empty()) return;
    try {
        if (!threads_.get_thread(thread_id)) return;   // deleted mid-turn
        const std::string msg_id = threads_.add_message(
            thread_id, "agent", agent_id, "*(attached a file)*",
            attachments_json);
        // Same correlation trail as complete_turn's reply journal; the
        // stored body stays raw and no user-facing surface renders it.
        std::fprintf(stderr, "[header] %sthread %.8s task %.12s "
            "(mid-turn attachment)\n",
            hivemind::render_message_header(
                std::string(hivemind::msg_source::kAgentPrefix) +
                    agent_id,
                hivemind::db::iso_now()).c_str(),
            thread_id.c_str(), task_id.c_str());
        // Posting implies read-to-own-message (same as complete_turn) —
        // the attachment message must never count as unread for its
        // sender.
        if (auto m = threads_.get_message(msg_id))
            deliveries_.mark_read(thread_id, agent_id, msg_id,
                                  m->created_at);
    } catch (const std::exception& e) {
        std::fprintf(stderr,
            "[chat] ERROR posting mid-turn attachment to thread %.8s: %s\n",
            thread_id.c_str(), e.what());
    }
}

void ChatService::complete_turn(const std::string& task_id,
                                const std::string& body,
                                const std::string& error,
                                const std::string& attachments_json,
                                const std::string& frame_thread_id,
                                const std::string& frame_agent_id,
                                const std::string& stats_json) {
    std::shared_ptr<LiveTurn> live;
    bool suppress = false;   // 2026-10-01: cancel raced with completion
    {
        std::lock_guard<std::mutex> lk(m_);
        auto it = by_task_.find(task_id);
        if (it != by_task_.end()) {
            live = it->second;
            by_task_.erase(it);
            if (live->cancelling) {
                // cancel_thread/cancel_agent_turns got here first: the
                // cancel path owns the user-facing record (delivery row
                // dropped "cancelled" + flush_partial's *(interrupted)*
                // note). Appending the full body too double-posts the
                // reply (Bob 2026-10-01: both copies landed 1s apart).
                // Consume the tombstone so a THIRD late frame can't
                // orphan-append either, and keep the row's cancelled
                // state instead of the tail's mark_done.
                cancelled_tasks_.erase(task_id);
                live->done = true;
                suppress = true;
            } else {
                // Claim the single reply slot BEFORE appending so a
                // cancel landing mid-append can't flush a duplicate.
                live->reply_posted = true;
            }
        }
    }
    if (suppress) {
        // Phase 67.2 still holds: Stop releases the durable queue even
        // when the cancelled turn's own result is what freed the agent.
        try_dispatch(live->agent_id);
        return;
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
    std::string last_reply_id;   // for fan-in source tagging (Phase 66b)
    if ((!out.empty() && out != "(agent produced no output)") || has_atts) {
        bool posted = false;
        for (int attempt = 0; attempt < 2 && !posted; ++attempt) {
            try {
                last_reply_id = threads_.add_message(thread_id, "agent",
                                                     agent_id, out,
                                                     attachments_json,
                                                     stats_json);
                posted = true;
            } catch (const std::exception& e) {
                std::fprintf(stderr,
                    "[chat] ERROR posting reply to thread %.8s "
                    "(attempt %d): %s\n",
                    thread_id.c_str(), attempt + 1, e.what());
            }
        }
        if (!posted) {
            live->error = "reply generated but could not be posted";
        } else {
            // 2026-10-04 (Plan.md §3.6): the response side of the
            // header trail — every posted reply is journaled with a
            // rendered [agent:<name> @ <time>] stamp so a reply can
            // always be correlated with the wake that produced it.
            // Hub journal only; the stored body stays raw and no
            // user-facing surface renders this.
            std::fprintf(stderr, "[header] %sthread %.8s task %.12s\n",
                hivemind::render_message_header(
                    std::string(hivemind::msg_source::kAgentPrefix) +
                        agent_name,
                    hivemind::db::iso_now()).c_str(),
                thread_id.c_str(), task_id.c_str());
            // Relay fires on ANY posted reply — the 2026-09-23 probe
            // proved the bug: Honey's "RELAY-PROBE-OK … acknowledged"
            // reply was ack-only, and with the hook behind the ack guard
            // the answer never reached the user's DM. An ack-looking peer
            // reply IS the answer the user is waiting for; ack-only must
            // gate ONLY the fan-in continuation below (65c).
            // Read receipts: posting implies read-to-own-message.
            if (auto m = threads_.get_message(last_reply_id))
                deliveries_.mark_read(thread_id, agent_id, last_reply_id,
                                      m->created_at);
            // Single wake path (Phase 66b): when a relay fires for
            // this reply, the requester's pair-thread fan-in below is
            // suppressed — the relay wake in the home thread is the
            // ONLY wake (kills the 2026-09-25 duplicate deliveries).
            const std::string relayed_to =
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
                            if (pid == relayed_to) continue;
                            auto peer = agents_.get_agent(pid);
                            if (peer)
                                fan_in(thread_id, *peer, agent_name,
                                       std::string(
                                           hivemind::msg_source::kAgentPrefix) +
                                           agent_name,
                                       out, generation + 1, agent_id,
                                       last_reply_id, "wake");
                        }
                    } else {
                        // 2-agent threads: broadcast is self-damping
                        // (branching factor 1) — keep it.
                        for (const auto& peer_id : thread->agent_ids) {
                            if (peer_id == agent_id) continue;
                            if (peer_id == relayed_to) continue;
                            auto peer = agents_.get_agent(peer_id);
                            if (peer)
                                fan_in(thread_id, *peer, agent_name,
                                       std::string(
                                           hivemind::msg_source::kAgentPrefix) +
                                           agent_name,
                                       out, generation + 1, agent_id,
                                       last_reply_id, "wake");
                        }
                    }
                }
            } catch (...) { /* thread deleted mid-turn */ }
            }
        }
    }
    live->done = true;
    // Phase 66b: close out the delivery row that drove this turn (if
    // any) and advance the agent's read mark to the wake's source
    // message — the agent demonstrably consumed it (it WAS the prompt).
    // Queue-join (Plan.md, 2026-10-04): one task may carry several
    // joined generation-0 rows — close out EVERY row, advancing the
    // read mark past each wake's source message.
    for (auto& d : deliveries_.find_all_by_task(task_id)) {
        deliveries_.mark_done(d.id);
        if (!d.source_message_id.empty())
            if (auto m = threads_.get_message(d.source_message_id))
                deliveries_.mark_read(thread_id, agent_id,
                                      d.source_message_id, m->created_at);
    }
    // Agent idle: dispatch the next queued wake, if any (durable queue).
    try_dispatch(agent_id);
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
        // Phase 66b: a force-retired turn FAILS its delivery row —
        // visible in message_status, not tombstoned-silent — and the
        // freed agent drains its next queued wake.
        for (const auto& v : victims) {
            for (auto& d : deliveries_.find_all_by_task(v.task_id))
                deliveries_.mark_error(d.id,
                    v.was_cancelling ? "cancel unconfirmed (watchdog)"
                                     : "turn timeout (watchdog)");
            try_dispatch(v.agent_id);
        }
        // TTL sweeps: old queued wakes and relay links become visible
        // terminal states instead of silently vanishing.
        const int expired = deliveries_.expire_queued(wake_ttl_s());
        if (expired > 0)
            std::fprintf(stderr,
                "[delivery] %d queued wake(s) expired (ttl %ds)\n",
                expired, wake_ttl_s());
        deliveries_.expire_relays(static_cast<int>(relay_ttl().count()));
    }
}

void ChatService::flush_partial(LiveTurn& t) {
    {
        std::lock_guard<std::mutex> lk(m_);
        // complete_turn may have won the race and already posted the
        // full reply (it claims reply_posted under m_ first).
        if (t.reply_posted || t.partial_flushed || t.partial.empty())
            return;
        t.partial_flushed = true;
    }
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
    std::vector<std::string> cancelled_ids;
    int count = 0;
    {
        std::lock_guard<std::mutex> lk(m_);
        for (auto& [thread_id, turns] : live_) {
            for (auto& t : turns) {
                if (t->agent_id != agent_id || t->done || t->cancelling)
                    continue;
                t->cancelling = true;
                t->cancel_started = std::chrono::steady_clock::now();
                if (!t->task_id.empty()) {
                    cancelled_tasks_.insert(t->task_id);
                    cancelled_ids.push_back(t->task_id);
                }
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
    // Phase 66b: cancelled turns' delivery rows become visibly dropped.
    // Queue-join: one task may own several joined rows — drop them all.
    for (const auto& id : cancelled_ids)
        for (auto& d : deliveries_.find_all_by_task(id))
            deliveries_.mark_dropped(d.id, "cancelled");
    // Same ordering as cancel_thread: cancels out of the lock, then
    // flush so the interrupt note lands after any in-flight text.
    for (const auto& [a, tid] : to_cancel) hub_.cancel_agent(a, tid);
    for (auto& t : to_flush) flush_partial(*t);
    return count;
}

void ChatService::cancel_thread(const std::string& thread_id) {
    std::vector<RosterAgent> to_cancel;
    std::vector<std::shared_ptr<LiveTurn>> to_flush;
    std::vector<std::string> cancelled_ids;
    {
        std::lock_guard<std::mutex> lk(m_);
        auto it = live_.find(thread_id);
        if (it == live_.end()) return;
        for (auto& t : it->second) {
            if (t->done || t->cancelling) continue;
            t->cancelling = true;
            t->cancel_started = std::chrono::steady_clock::now();
            if (!t->task_id.empty()) {
                cancelled_tasks_.insert(t->task_id);
                cancelled_ids.push_back(t->task_id);
            }
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
    // Phase 66b: cancelled turns' delivery rows become visibly dropped.
    // Queue-join: one task may own several joined rows — drop them all.
    for (const auto& id : cancelled_ids)
        for (auto& d : deliveries_.find_all_by_task(id))
            deliveries_.mark_dropped(d.id, "cancelled");
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
