// tests/test_agent_relay.cpp — origin-linked relay (2026-09-23): an
// agent that calls message_agent DURING a user-DM turn gets the peer's
// substantive reply mirrored back into the user DM (and is woken there
// to frame it). Uses a capturing HubRouter mock — no worker needed.
#include "hivemind/ChatService.h"
#include "hivemind/HubRouter.h"
#include "hivemind/AgentStore.h"
#include "hivemind/ThreadStore.h"
#include "hivemind/DeliveryStore.h"
#include "hivemind/WorkerChannel.h"
#include "hivemind/WorkerStore.h"
#include "hivemind/RoutineStore.h"
#include "hivemind/SecretStore.h"

#include <algorithm>
#include <cassert>
#include <filesystem>
#include <iostream>
#include <vector>

namespace fs = std::filesystem;

namespace {

struct Dispatch {
    std::string agent_id, thread_id, task_id;
    std::string prompt;   // 2026-10-04: capture for header assertions
};

class MockHub final : public HubRouter {
public:
    using HubRouter::HubRouter;
    std::vector<Dispatch> dispatches;
    int next = 0;
    // 2026-10-22 (bug sweep) test hooks:
    std::string throw_on;         // B1: dispatch throws when the prompt
                                  // contains this (simulated poison dump)
    ChatService* chat = nullptr;  // B2: reentrancy hook
    std::string reenter_agent;    // B2: reenter try_dispatch mid-dispatch
    bool reentering = false;
    std::string dispatch(const RosterAgent& agent,
                         const std::string& thread_id,
                         const std::string& text) override {
        if (!throw_on.empty() && text.find(throw_on) != std::string::npos)
            throw std::runtime_error("simulated poison dispatch");
        std::string tid = "mock-task-" + std::to_string(++next);
        dispatches.push_back({agent.id, thread_id, tid, text});
        // B2: simulate the concurrent caller that passed the idle check
        // before the winner claimed the turn — old code double-dispatched
        // the same queued row from here.
        if (chat && !reenter_agent.empty() && agent.id == reenter_agent &&
            !reentering) {
            reentering = true;
            chat->on_agent_ready(agent.id);   // public try_dispatch entry
            reentering = false;
        }
        return tid;
    }
    const Dispatch* find(const std::string& agent_id,
                         const std::string& thread_id) const {
        for (const auto& d : dispatches)
            if (d.agent_id == agent_id && d.thread_id == thread_id)
                return &d;
        return nullptr;
    }
    const Dispatch* find_last(const std::string& agent_id,
                              const std::string& thread_id) const {
        const Dispatch* hit = nullptr;
        for (const auto& d : dispatches)
            if (d.agent_id == agent_id && d.thread_id == thread_id)
                hit = &d;
        return hit;
    }
};

int count_from(ThreadStore& threads, const std::string& tid,
               const std::string& sender_id) {
    int n = 0;
    for (const auto& m : threads.list_messages(tid))
        if (m.sender_kind == "agent" && m.sender_id == sender_id) ++n;
    return n;
}

// Complete every live turn with an ack-only reply ("acknowledged" gates
// the fan-in continuation, 65c) until all agents are idle. The
// busy-receiver hold queue (2026-09-25) defers fan-in wakes while the
// target has any live turn; drained wakes surface as new live turns on
// later passes. Steps that assume an idle starting state call this
// first — agent_activity() is re-queried each pass.
void settle(ChatService& chat) {
    for (int guard = 0; guard < 32; ++guard) {
        const auto live = chat.agent_activity();
        if (live.empty()) return;
        for (const auto& [_, t] : live)
            if (!t.task_id.empty())
                chat.complete_turn(t.task_id, "acknowledged.", "");
    }
}

} // namespace

int main() {
    setenv("HIVEMIND_RELAY_TTL_S", "900", 1);

    fs::path tmp = fs::temp_directory_path() / "hivemind_test_agent_relay";
    fs::remove_all(tmp);
    fs::create_directories(tmp);

    AgentStore agents{tmp / "agents.db"};
    ThreadStore threads{tmp / "threads.db"};
    RoutineStore routines{tmp / "routines.db"};
    WorkerStore wstore{tmp / "workers.db"};
    WorkerChannel wchannel{wstore, 0, "127.0.0.1"};
    hivemind::SecretStore secrets{tmp / "secrets.db",
                                  tmp / "secrets.key"};
    MockHub hub{agents, threads, wchannel, routines, secrets};
    hivemind::DeliveryStore deliveries{tmp / "deliveries.db"};
    ChatService chat{agents, threads, hub, deliveries};
    hub.set_chat_service(&chat);

    const std::string wkey = wstore.enroll("testrig");
    (void)wkey;
    auto chief = agents.create_agent("Chief", "", "", "", "",
                                     "testrig");
    auto honey = agents.create_agent("Honey", "", "", "", "",
                                     "testrig");

    // User DM with Chief (kind "dm", one agent).
    auto dm = threads.create_thread("Chief", "dm", {chief.id});

    // 1) User asks Chief; Chief's turn goes live.
    chat.post_user_message(dm.id, "ask Honey for the launch numbers");
    const auto* user_turn_p = hub.find(chief.id, dm.id);
    assert(user_turn_p && "user message should dispatch to Chief");
    // NB: capture strings, not pointers — hub.dispatches reallocs.
    const std::string user_task = user_turn_p->task_id;
    // 2026-10-04 (Plan.md): every dispatched wake carries a rendered
    // header from the STORED source + created_at. The user post's
    // prompt must open with the user-source header and the stored body
    // must NOT contain it (visibility boundary §3.6).
    assert(user_turn_p->prompt.rfind("[user @ ", 0) == 0 &&
           "user wake prompt must open with the [user @ …] header");
    assert(user_turn_p->prompt.find("ask Honey for the launch numbers")
           != std::string::npos);
    {
        bool clean = true;
        for (const auto& m : threads.list_messages(dm.id))
            if (m.sender_kind == "user" &&
                m.body.rfind("[user @ ", 0) == 0)
                clean = false;
        assert(clean && "stored user body must stay header-free");
    }

    // 2) Mid-turn, Chief messages Honey (pair thread created).
    chat.post_agent_message(chief.id, honey.id, "what are the numbers?");
    // relay must be armed; Honey's pair turn dispatched.
    std::string pair_tid, pair_task;
    for (const auto& d : hub.dispatches)
        if (d.agent_id == honey.id && d.thread_id != dm.id) {
            pair_tid = d.thread_id; pair_task = d.task_id;
        }
    assert(!pair_task.empty() && "message_agent should fan in to Honey");
    // message_agent wakes carry the agent-source header.
    {
        const auto* pair = hub.find(honey.id, pair_tid);
        assert(pair && pair->prompt.rfind("[agent:Chief @ ", 0) == 0 &&
               "agent wake prompt must open with [agent:Chief @ …]");
    }

    // 3) Chief's user turn completes ("I've asked Honey") — no relay.
    chat.complete_turn(user_task, "I've asked Honey, hold on.", "");
    assert(count_from(threads, dm.id, honey.id) == 0);

    // 4) Honey answers in the pair thread → relay fires: mirror into the
    //    user DM + wake Chief there.
    chat.complete_turn(pair_task, "Q3 numbers: 42 units shipped.", "");
    assert(count_from(threads, dm.id, honey.id) == 1);
    // the SECOND Chief dispatch in the dm thread is the relay wake:
    int chief_dm_dispatches = 0;
    for (const auto& d : hub.dispatches)
        if (d.agent_id == chief.id && d.thread_id == dm.id)
            ++chief_dm_dispatches;
    assert(chief_dm_dispatches == 2 && "relay should re-dispatch Chief in the user DM");
    {
        const auto* relay = hub.find_last(chief.id, dm.id);
        assert(relay && relay->prompt.rfind("[agent:Honey @ ", 0) == 0 &&
               "relay wake must carry the replier's agent-source header");
    }

    // 5) Chief's framing reply completes in the DM — no further fan-in
    //    (dm thread has no peers; relay is one-shot). Dispatches so far:
    //    1=user turn, 2=Honey pair, 3=relay wake. Phase 66b: the pair
    //    fan-in to Chief is SUPPRESSED when the relay fires (single
    //    wake path — the old dispatch #4 was the 2026-09-25 duplicate).
    chat.complete_turn("mock-task-3", "Honey says 42 units shipped.", "");
    assert(hub.dispatches.size() == 3);

    // 6) One-shot: a SECOND Honey pair-turn reply must NOT mirror again.
    //    (Simulate continued pair chatter without a new user-behalf ask.)
    chat.post_agent_message(chief.id, honey.id, "thanks!");
    // ^ arms a NEW link? Chief has no live user-DM turn now → must NOT arm.
    const auto* pair_turn2 = hub.find_last(honey.id, pair_tid);
    assert(pair_turn2 && pair_turn2->task_id != pair_task);
    chat.complete_turn(pair_turn2->task_id, "anytime!", "");
    assert(count_from(threads, dm.id, honey.id) == 1);   // unchanged

    // 7) Agent chatter from a PAIR-thread turn never arms a relay.
    //    (Honey, mid pair turn, messages Chief — Honey's live turn is in
    //    the pair thread, not a user DM.)
    //    Simulate: Chief user turn live again, but this time the
    //    agent_msg comes from Honey whose live turn is the pair thread.
    // 2026-09-25: with the hold queue, Chief's stale pair turn (left
    //    open by step 5's drain) would hold this user wake instead of
    //    dispatching it — settle to the intended idle state first.
    settle(chat);
    chat.post_user_message(dm.id, "another question");
    const auto* t7 = hub.find_last(chief.id, dm.id);
    assert(t7);
    const std::string step7_task = t7->task_id;   // left open: Chief busy
    chat.post_agent_message(honey.id, chief.id, "unsolicited FYI");
    // Honey's FYI -> pair thread; Chief's pair reply must not mirror.
    const auto* chief_pair = hub.find_last(chief.id, pair_tid);
    assert(chief_pair);
    const std::string chief_pair_task = chief_pair->task_id;
    chat.complete_turn(chief_pair_task, "got it, thanks Honey", "");
    // DM still holds only: Chief's "I've asked", Honey's mirror, Chief's
    // framing — pair chatter never leaked in.
    assert(count_from(threads, dm.id, chief.id) == 2);
    assert(count_from(threads, dm.id, honey.id) == 1);

    // 8) REGRESSION (2026-09-23 probe): an ack-only-looking peer reply
    //    ("acknowledged" + <=600 chars) must STILL relay — ack-only gates
    //    fan-in continuation, never the relay. Production failure mode.
    // Held wakes from step 7 (the FYI fan-in found Chief busy) must
    // drain before this step's relay choreography — settle first.
    settle(chat);
    chat.post_user_message(dm.id, "ask Honey for the probe word");
    const auto* t8 = hub.find_last(chief.id, dm.id);
    assert(t8);
    chat.post_agent_message(chief.id, honey.id, "probe word please");
    const auto* honey8 = hub.find_last(honey.id, pair_tid);
    assert(honey8 && honey8->task_id != pair_task);
    const std::string honey8_task = honey8->task_id;
    chat.complete_turn(t8->task_id, "asking her now.", "");
    const int honey_in_dm_before = count_from(threads, dm.id, honey.id);
    chat.complete_turn(honey8_task,
        "RELAY-PROBE-OK — acknowledged and answered.", "");   // ack-only!
    assert(count_from(threads, dm.id, honey.id) ==
           honey_in_dm_before + 1);   // ack-only reply STILL mirrored


    // 9) BUSY REQUESTER (2026-09-24): the peer's reply arrives while the
    //    requester still has a live turn — the reply must mirror into
    //    the DM immediately, but the wake dispatch must be HELD until
    //    the requester goes idle (previously the queued wake aged out
    //    under the turn watchdog and was silently tombstoned).
    // 2026-09-25 rework: the hold queue now also defers pair-thread
    // continuations, so leftover turns from steps 7-8 would hold this
    // step's wakes — settle to a truly idle state first. Drain ordering
    // on completion is relay queue first, then one held message wake.
    settle(chat);
    chat.post_user_message(dm.id, "ask Honey for the busy-test word");
    const auto* t9 = hub.find_last(chief.id, dm.id);
    assert(t9);
    const std::string chief_busy_task = t9->task_id;
    chat.post_agent_message(chief.id, honey.id, "busy-test word please");
    const auto* honey9 = hub.find_last(honey.id, pair_tid);
    assert(honey9);
    const std::string honey9_task = honey9->task_id;
    // Honey answers while Chief's user-DM turn is STILL live (busy).
    int chief_dm_dispatches_before = 0;
    for (const auto& d : hub.dispatches)
        if (d.agent_id == chief.id && d.thread_id == dm.id)
            ++chief_dm_dispatches_before;
    chat.complete_turn(honey9_task, "the busy-test word is ZEBRA.", "");
    // Mirror lands immediately even though Chief is busy...
    assert(count_from(threads, dm.id, honey.id) >= 1);
    // ...but NO new wake dispatch while Chief is busy.
    int chief_dm_dispatches_busy = 0;
    for (const auto& d : hub.dispatches)
        if (d.agent_id == chief.id && d.thread_id == dm.id)
            ++chief_dm_dispatches_busy;
    assert(chief_dm_dispatches_busy == chief_dm_dispatches_before &&
           "wake must not dispatch to a busy requester");
    // Chief's long turn finally completes: idle → the held RELAY wake
    // drains exactly once (re-busying Chief), while Honey's pair
    // continuation stays held in the message-wake queue.
    chat.complete_turn(chief_busy_task, "sorry, that took a while.", "");
    {
        int n = 0;
        for (const auto& d : hub.dispatches)
            if (d.agent_id == chief.id && d.thread_id == dm.id) ++n;
        assert(n == chief_dm_dispatches_before + 1 &&
               "idle requester drains the held relay wake exactly once");
    }
    // Completing the drained relay wake idles Chief again, releasing
    // the held pair continuation; settle until truly idle.
    settle(chat);
    assert(chat.agent_activity().count(chief.id) == 0 &&
           "settle loop should fully idle Chief");

    // 10) BUSY RECEIVER hold queue (2026-09-25, Chief's relay bug
    //     report): wakes to a mid-turn agent must be held hub-side and
    //     drained on idle — previously they dispatched into the agent's
    //     serial queue while the hub-side LiveTurn aged from dispatch
    //     and was watchdog-tombstoned, silently dropping the answer.
    // Honey -> Chief while Chief is idle: dispatches immediately.
    chat.post_agent_message(honey.id, chief.id, "hold-test one");
    const auto* h10 = hub.find_last(chief.id, pair_tid);
    assert(h10);
    const std::string chief10_task = h10->task_id;   // left open: busy
    int chief_pair_dispatches = 0;
    for (const auto& d : hub.dispatches)
        if (d.agent_id == chief.id && d.thread_id == pair_tid)
            ++chief_pair_dispatches;

    // Honey -> Chief while Chief is busy: HELD, no new dispatch.
    chat.post_agent_message(honey.id, chief.id, "hold-test two");
    int n_held = 0;
    for (const auto& d : hub.dispatches)
        if (d.agent_id == chief.id && d.thread_id == pair_tid) ++n_held;
    assert(n_held == chief_pair_dispatches &&
           "wake must be held while the receiver is busy");

    // Dedup (09-23 double-delivery): the identical body again inside
    // the window is a retry — dropped BEFORE posting, so the thread
    // message count must not grow (and no second wake is queued).
    const int pair_msgs_before =
        (int)threads.list_messages(pair_tid).size();
    chat.post_agent_message(honey.id, chief.id, "hold-test two");
    assert((int)threads.list_messages(pair_tid).size() ==
               pair_msgs_before &&
           "identical retry must be deduped");

    // Chief's long turn completes (ack-only gates the continuation) —
    // the held wake drains exactly once.
    chat.complete_turn(chief10_task, "acknowledged.", "");
    n_held = 0;
    const Dispatch* drained = nullptr;
    for (const auto& d : hub.dispatches)
        if (d.agent_id == chief.id && d.thread_id == pair_tid) {
            ++n_held;
            drained = &d;
        }
    assert(n_held == chief_pair_dispatches + 1 &&
           "held wake must drain exactly once when the receiver idles");
    assert(drained && drained->task_id != chief10_task);

    // User messages hold too (same serial-agent tombstone): with Chief
    // busy on the drained wake, a user DM queues instead of dispatching.
    int dm_before = 0;
    for (const auto& d : hub.dispatches)
        if (d.agent_id == chief.id && d.thread_id == dm.id) ++dm_before;
    chat.post_user_message(dm.id, "hello while you are busy");
    int dm_now = 0;
    for (const auto& d : hub.dispatches)
        if (d.agent_id == chief.id && d.thread_id == dm.id) ++dm_now;
    assert(dm_now == dm_before && "user wake must hold while busy");
    // Completing the drained pair turn idles Chief → user wake drains.
    chat.complete_turn(drained->task_id, "acknowledged.", "");
    dm_now = 0;
    for (const auto& d : hub.dispatches)
        if (d.agent_id == chief.id && d.thread_id == dm.id) ++dm_now;
    assert(dm_now == dm_before + 1 && "held user wake must drain on idle");

    // Settle everything; both agents fully idle at the end.
    settle(chat);
    assert(chat.agent_activity().empty());

    // 11) Phase 66d: confirmed send returns the durable delivery id;
    //     delivery_status exposes the state machine; delivery_is_read
    //     flips once the target's turn consumes the wake.
    std::string did;
    chat.post_agent_message(chief.id, honey.id, "status probe", &did);
    assert(!did.empty() && "confirmed send must return a delivery id");
    auto st = chat.delivery_status(chief.id, did);
    assert(st.size() == 1 && st[0].id == did);
    assert(st[0].state == "dispatched" && "idle target dispatches at once");
    assert(!chat.delivery_is_read(st[0]) && "not read before the turn");
    // Privacy: a third party (neither sender nor target) sees nothing.
    assert(chat.delivery_status("not-a-party", did).empty());
    // Complete the target's turn -> done + read receipt.
    const auto* h11 = hub.find_last(honey.id, pair_tid);
    assert(h11);
    chat.complete_turn(h11->task_id, "acknowledged.", "");
    st = chat.delivery_status(chief.id, did);
    assert(st.size() == 1 && st[0].state == "done");
    assert(chat.delivery_is_read(st[0]) && "read after the turn ran");
    // No-id query: the agent's recent sends, newest first.
    auto recent = chat.delivery_status(chief.id);
    assert(!recent.empty() && recent[0].id == did);
    settle(chat);

    // 12) Phase 66c: the human user's view mark is isolated from agent
    //     delivery receipts. Simulates the user watching the pair
    //     thread in the client (POST /api/threads/:id/read).
    std::string did2;
    chat.post_agent_message(chief.id, honey.id, "user is watching", &did2);
    auto st2 = chat.delivery_status(chief.id, did2);
    assert(st2.size() == 1 && !chat.delivery_is_read(st2[0]) &&
           "fresh delivery unread");
    chat.user_mark_read(pair_tid);   // the user "views" the thread
    st2 = chat.delivery_status(chief.id, did2);
    assert(!chat.delivery_is_read(st2[0]) &&
           "user view must NOT flip an agent-addressed delivery receipt");
    assert(chat.read_mark(pair_tid, "user").has_value() &&
           "the user's own mark advanced (66c badge basis)");
    // 66c unread badges: 0 right after viewing, +1 per non-user
    // message, and the user's own messages never count.
    assert(chat.unread_for_user(pair_tid) == 0 &&
           "viewed thread has nothing unread");
    chat.post_user_message(pair_tid, "my own words are never unread");
    assert(chat.unread_for_user(pair_tid) == 0 &&
           "user-sent messages do not count as unread");
    // The agent receipt path is intact: the target's own turn flips it.
    const auto* h12 = hub.find_last(honey.id, pair_tid);
    assert(h12);
    chat.complete_turn(h12->task_id, "acknowledged.", "");
    st2 = chat.delivery_status(chief.id, did2);
    assert(chat.delivery_is_read(st2[0]) && "agent receipt path intact");
    // Honey's reply lands after the user's mark → exactly 1 unread.
    assert(chat.unread_for_user(pair_tid) == 1 &&
           "one agent reply after the mark = one unread");
    // No mark at all → every non-user message counts (first-run
    // truth). The user-inbox thread is never user-marked in this test.
    const std::string inbox_tid =
        chat.post_user_inbox_message(chief.id, "inbox probe for unread");
    assert(!chat.read_mark(inbox_tid, "user").has_value());
    assert(chat.unread_for_user(inbox_tid) >= 1 &&
           "never-viewed thread counts all non-user messages");
    // 13) 2026-10-04 (Plan.md): source-typed wakes render the right
    //     header AND the voice channel hint. The voice hint moved
    //     server-side from the Android OUTGOING_PREFIX — it rides the
    //     PROMPT only, never the stored body (§3.6 boundary).
    settle(chat);
    chat.post_user_message(dm.id, "voice check one two", "[]", "voice-app");
    {
        const auto* v = hub.find_last(chief.id, dm.id);
        assert(v && v->prompt.rfind("[voice-app @ ", 0) == 0 &&
               "voice wake must open with [voice-app @ …]");
        assert(v->prompt.find("text-to-speech") != std::string::npos &&
               "voice wake must carry the TTS channel hint");
        bool clean = true;
        for (const auto& m : threads.list_messages(dm.id))
            if (m.sender_kind == "user" &&
                m.body.find("text-to-speech") != std::string::npos)
                clean = false;
        assert(clean && "TTS hint must never reach the stored body");
    }
    // Routine fires carry routine:<label>; nudges carry system.
    chat.complete_turn(hub.find_last(chief.id, dm.id)->task_id, "ok", "");
    settle(chat);
    chat.post_user_message(dm.id, "routine probe", "[]",
                           "routine:social-warmup-daily");
    {
        const auto* r = hub.find_last(chief.id, dm.id);
        assert(r && r->prompt.rfind("[routine:social-warmup-daily @ ", 0) == 0 &&
               "routine wake must open with [routine:<label> @ …]");
        chat.complete_turn(r->task_id, "routine ack", "");
    }
    settle(chat);
    chat.post_nudge(dm.id, "nudge body");
    {
        const auto* n = hub.find_last(chief.id, dm.id);
        assert(n && n->prompt.rfind("[system @ ", 0) == 0 &&
               "nudge wake must open with [system @ …]");
        chat.complete_turn(n->task_id, "nudged", "");
    }
    settle(chat);

    // ── 2026-10-04 queue-join (Plan.md) ──────────────────────────────
    // Burst 3 gen-0 messages while Chief is busy → they queue; when the
    // live turn completes they must drain as ONE joined prompt (one
    // task, per-message headers, FIFO order), and every joined row
    // lands 'done' on that single turn.
    settle(chat);
    chat.post_user_message(dm.id, "join-1 first");
    const auto* t1 = hub.find_last(chief.id, dm.id);
    assert(t1 && "first burst message should dispatch immediately");
    const std::string task1 = t1->task_id;
    const size_t dm_dispatches_before =
        static_cast<size_t>(std::count_if(hub.dispatches.begin(),
            hub.dispatches.end(), [&](const Dispatch& d) {
                return d.agent_id == chief.id && d.thread_id == dm.id;
            }));
    // Chief is BUSY now — the next two queue instead of dispatching.
    chat.post_user_message(dm.id, "join-2 amendment");
    chat.post_user_message(dm.id, "join-3 correction: undo join-2");
    {
        size_t dm_dispatches_now = static_cast<size_t>(std::count_if(
            hub.dispatches.begin(), hub.dispatches.end(),
            [&](const Dispatch& d) {
                return d.agent_id == chief.id && d.thread_id == dm.id;
            }));
        assert(dm_dispatches_now == dm_dispatches_before &&
               "bursted wakes must queue while the agent is busy");
        int queued = 0;
        for (const auto& d : deliveries.list(chief.id, "queued"))
            if (d.thread_id == dm.id) ++queued;
        assert(queued == 2 && "both burst wakes must sit queued");
    }
    // Complete turn 1 → try_dispatch must JOIN the two queued wakes.
    chat.complete_turn(task1, "ack", "");
    {
        const auto* joined = hub.find_last(chief.id, dm.id);
        assert(joined && joined->task_id != task1 &&
               "queued wakes must drain as a second dispatch");
        assert(joined->prompt.rfind("[user @ ", 0) == 0 &&
               "joined prompt opens with the first queued header");
        const auto pos2 = joined->prompt.find("join-2 amendment");
        const auto pos3 = joined->prompt.find("join-3 correction");
        assert(pos2 != std::string::npos && pos3 != std::string::npos &&
               "joined prompt must carry both queued bodies");
        assert(pos2 < pos3 && "joined bodies keep FIFO order");
        assert(joined->prompt.find("[user @ ", pos2) != std::string::npos &&
               "each joined segment keeps its own header");
        // Every joined row shares the one task and completes on it.
        chat.complete_turn(joined->task_id, "joined ack", "");
        int done = 0;
        for (const auto& d : deliveries.list(chief.id, "done"))
            if (d.thread_id == dm.id &&
                (d.body == "join-2 amendment" ||
                 d.body == "join-3 correction: undo join-2"))
                ++done;
        assert(done == 2 && "both joined rows must land done on one turn");
    }
    // Cross-thread isolation: a queued wake in another thread must NOT
    // merge into the dm drain.
    settle(chat);
    auto dm2 = threads.create_thread("Chief second", "dm", {chief.id});
    chat.post_user_message(dm2.id, "other-thread first");
    const auto* o1 = hub.find_last(chief.id, dm2.id);
    assert(o1 && "other-thread message dispatches on its own");
    chat.post_user_message(dm.id, "dm-queued-while-other-busy");
    chat.complete_turn(o1->task_id, "other ack", "");
    {
        const auto* back = hub.find_last(chief.id, dm.id);
        assert(back && back->prompt.find("dm-queued-while-other-busy")
               != std::string::npos &&
               "dm wake drains separately from the other thread");
        assert(back->prompt.find("other-thread") == std::string::npos &&
               "no cross-thread merge");
        chat.complete_turn(back->task_id, "dm ack", "");
    }
    settle(chat);

    // ── 2026-10-04 gen>0 queue-join (lee): agent fan-in bursts join
    // into one turn exactly like user bursts. ──
    settle(chat);
    auto honey_dm = threads.create_thread("Honey dm", "dm", {honey.id});
    chat.post_user_message(honey_dm.id, "hold a moment");
    const std::string honey_task1 =
        hub.find_last(honey.id, honey_dm.id)->task_id;
    const size_t dispatches_before = hub.dispatches.size();
    chat.post_agent_message(chief.id, honey.id, "fan-a first question");
    chat.post_agent_message(chief.id, honey.id, "fan-b follow-up");
    chat.post_agent_message(chief.id, honey.id,
                            "fan-c correction: skip fan-b");
    assert(hub.dispatches.size() == dispatches_before &&
           "fan-in wakes to a busy agent must queue, not dispatch");
    {
        int queued = 0;
        for (const auto& d : deliveries.list(honey.id, "queued"))
            if (d.body.rfind("fan-", 0) == 0) ++queued;
        assert(queued == 3 && "all three fan-in wakes sit queued");
    }
    // Honey's user turn ends -> the 3 queued gen-1 wakes must JOIN.
    chat.complete_turn(honey_task1, "standing by", "");
    {
        assert(hub.dispatches.size() == dispatches_before + 1 &&
               "three queued fan-in wakes must join into ONE dispatch");
        const auto& j = hub.dispatches.back();
        assert(j.agent_id == honey.id && "joined wake targets Honey");
        const auto pa = j.prompt.find("fan-a first question");
        const auto pb = j.prompt.find("fan-b follow-up");
        const auto pc = j.prompt.find("fan-c correction");
        assert(pa != std::string::npos && pb != std::string::npos &&
               pc != std::string::npos &&
               "joined prompt carries all three fan-in bodies");
        assert(pa < pb && pb < pc && "FIFO order preserved");
        assert(j.prompt.rfind("[agent:Chief @ ", 0) == 0 &&
               "joined prompt opens with the agent-source header");
        assert(j.prompt.find("[agent:Chief @ ", pb) != std::string::npos &&
               "later segments keep their own headers");
        // "acknowledged." gates the 65c fan-in continuation.
        chat.complete_turn(j.task_id, "acknowledged.", "");
        int done = 0;
        for (const auto& d : deliveries.list(honey.id, "done"))
            if (d.body == "fan-a first question" ||
                d.body == "fan-b follow-up" ||
                d.body == "fan-c correction: skip fan-b")
                ++done;
        assert(done == 3 && "all three joined fan-in rows land done");
    }
    settle(chat);

    // ── 2026-10-22 bug sweep: B1 — poison dispatch containment ──────
    // A dispatch failure (WorkerChannel dump throwing on poison UTF-8 is
    // the production case) must mark the wake row error and MUST NOT
    // wedge the agent's queue: the next clean wake dispatches normally.
    {
        hub.throw_on = "THROW_ME";
        const size_t before_err =
            deliveries.list(chief.id, "error", 50).size();
        chat.post_user_message(dm.id, "THROW_ME poison marker");
        const auto errs = deliveries.list(chief.id, "error", 50);
        assert(errs.size() == before_err + 1 &&
               "poison wake must be marked error, not left queued");
        assert(errs.front().error.rfind("dispatch: ", 0) == 0 &&
               "error row must carry the dispatch failure reason");
        // Head-of-line check: a clean wake must dispatch immediately.
        chat.post_user_message(dm.id, "clean follow-up");
        const auto* clean = hub.find_last(chief.id, dm.id);
        assert(clean && clean->prompt.find("clean follow-up") !=
                            std::string::npos &&
               "clean wake after a poison wake must dispatch (no HOL block)");
        hub.throw_on.clear();
        settle(chat);
    }

    // ── 2026-10-22 bug sweep: B2 — no double-dispatch under race ─────
    // The mock re-enters try_dispatch from INSIDE dispatch — exactly the
    // interleaving of two real callers racing the idle check. Old code
    // dispatched the same queued row twice; the claim fix lets the
    // reentrant call see the agent busy and back off.
    {
        const size_t before = hub.dispatches.size();
        hub.chat = &chat;
        hub.reenter_agent = chief.id;
        chat.post_user_message(dm.id, "race probe");
        hub.reenter_agent.clear();
        hub.chat = nullptr;
        size_t n = 0;
        for (size_t i = before; i < hub.dispatches.size(); ++i)
            if (hub.dispatches[i].agent_id == chief.id &&
                hub.dispatches[i].thread_id == dm.id)
                ++n;
        assert(n == 1 && "racing try_dispatch must not double-dispatch");
        settle(chat);
    }

    // ── 2026-10-22 bug sweep: B5 — dedup returns the original handle ──
    {
        std::string did1, did2;
        chat.post_agent_message(chief.id, honey.id, "dedup-probe ping",
                                &did1);
        const std::string tid2 =
            chat.post_agent_message(chief.id, honey.id, "dedup-probe ping",
                                    &did2);
        assert(!did1.empty() && "first send gets a delivery id");
        assert(did2 == did1 &&
               "deduped retry must return the ORIGINAL delivery id");
        assert(!tid2.empty() &&
               "deduped retry must return the original pair thread");
        settle(chat);
    }

    std::cout << "agent relay: all assertions passed\n";
    return 0;
}
