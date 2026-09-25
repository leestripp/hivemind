// tests/test_agent_relay.cpp — origin-linked relay (2026-09-23): an
// agent that calls message_agent DURING a user-DM turn gets the peer's
// substantive reply mirrored back into the user DM (and is woken there
// to frame it). Uses a capturing HubRouter mock — no worker needed.
#include "hivemind/ChatService.h"
#include "hivemind/HubRouter.h"
#include "hivemind/AgentStore.h"
#include "hivemind/ThreadStore.h"
#include "hivemind/WorkerChannel.h"
#include "hivemind/WorkerStore.h"
#include "hivemind/ApprovalStore.h"
#include "hivemind/RoutineStore.h"
#include "hivemind/SecretStore.h"

#include <cassert>
#include <filesystem>
#include <iostream>
#include <vector>

namespace fs = std::filesystem;

namespace {

struct Dispatch { std::string agent_id, thread_id, task_id; };

class MockHub final : public HubRouter {
public:
    using HubRouter::HubRouter;
    std::vector<Dispatch> dispatches;
    int next = 0;
    std::string dispatch(const RosterAgent& agent,
                         const std::string& thread_id,
                         const std::string& /*text*/) override {
        std::string tid = "mock-task-" + std::to_string(++next);
        dispatches.push_back({agent.id, thread_id, tid});
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
    ApprovalStore approvals{tmp / "approvals.db"};
    RoutineStore routines{tmp / "routines.db"};
    WorkerStore wstore{tmp / "workers.db"};
    WorkerChannel wchannel{wstore, 0, "127.0.0.1"};
    hivemind::SecretStore secrets{tmp / "secrets.db",
                                  tmp / "secrets.key"};
    MockHub hub{agents, threads, wchannel, approvals, routines, secrets};
    ChatService chat{agents, threads, hub};
    hub.set_chat_service(&chat);

    const std::string wkey = wstore.enroll("testrig");
    (void)wkey;
    auto chief = agents.create_agent("Chief", "", "", "", false, "",
                                     "testrig");
    auto honey = agents.create_agent("Honey", "", "", "", false, "",
                                     "testrig");

    // User DM with Chief (kind "dm", one agent).
    auto dm = threads.create_thread("Chief", "dm", {chief.id});

    // 1) User asks Chief; Chief's turn goes live.
    chat.post_user_message(dm.id, "ask Honey for the launch numbers");
    const auto* user_turn_p = hub.find(chief.id, dm.id);
    assert(user_turn_p && "user message should dispatch to Chief");
    // NB: capture strings, not pointers — hub.dispatches reallocs.
    const std::string user_task = user_turn_p->task_id;

    // 2) Mid-turn, Chief messages Honey (pair thread created).
    chat.post_agent_message(chief.id, honey.id, "what are the numbers?");
    // relay must be armed; Honey's pair turn dispatched.
    std::string pair_tid, pair_task;
    for (const auto& d : hub.dispatches)
        if (d.agent_id == honey.id && d.thread_id != dm.id) {
            pair_tid = d.thread_id; pair_task = d.task_id;
        }
    assert(!pair_task.empty() && "message_agent should fan in to Honey");

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

    // 5) Chief's framing reply completes in the DM — no further fan-in
    //    (dm thread has no peers; relay is one-shot). Dispatches so far:
    //    1=user turn, 2=Honey pair, 3=relay wake, 4=pair fan-in to Chief
    //    (Honey's reply also continues the pair thread — normal flow).
    chat.complete_turn("mock-task-3", "Honey says 42 units shipped.", "");
    assert(hub.dispatches.size() == 4);

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

    std::cout << "agent relay: all assertions passed\n";
    return 0;
}
