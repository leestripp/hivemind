// tests/test_chat_throttle.cpp — Phase 65a/65b: Stop=mute + circuit
// breaker. Uses a real ChatService with a never-started WorkerChannel:
// dispatch() returns "" (worker offline) and fan_in posts a
// "dispatch failed" system message per attempt — counting those system
// messages measures exactly how many fan-in dispatches happened.
#include "hivemind/ChatService.h"
#include "hivemind/HubRouter.h"
#include "hivemind/AgentStore.h"
#include "hivemind/ThreadStore.h"
#include "hivemind/DeliveryStore.h"
#include "hivemind/WorkerChannel.h"
#include "hivemind/WorkerStore.h"
#include "hivemind/ApprovalStore.h"
#include "hivemind/RoutineStore.h"
#include "hivemind/SecretStore.h"

#include <cassert>
#include <cstdlib>
#include <filesystem>
#include <iostream>

namespace fs = std::filesystem;

namespace {

int dispatch_failures(ThreadStore& threads, const std::string& tid) {
    int n = 0;
    for (const auto& m : threads.list_messages(tid))
        if (m.sender_kind == "system" &&
            m.body.find("dispatch to agent") != std::string::npos)
            ++n;
    return n;
}

int breaker_notices(ThreadStore& threads, const std::string& tid) {
    int n = 0;
    for (const auto& m : threads.list_messages(tid))
        if (m.sender_kind == "system" &&
            m.body.find("circuit breaker tripped") != std::string::npos)
            ++n;
    return n;
}

} // namespace

int main() {
    // Tight breaker for the test; generous window so nothing expires.
    setenv("HIVEMIND_CB_MAX_TURNS", "3", 1);
    setenv("HIVEMIND_CB_WINDOW_S", "3600", 1);
    setenv("HIVEMIND_MUTE_S", "600", 1);

    fs::path tmp = fs::temp_directory_path() / "hivemind_test_chat_throttle";
    fs::remove_all(tmp);
    fs::create_directories(tmp);

    AgentStore agents{tmp / "agents.db"};
    ThreadStore threads{tmp / "threads.db"};
    ApprovalStore approvals{tmp / "approvals.db"};
    RoutineStore routines{tmp / "routines.db"};
    WorkerStore wstore{tmp / "workers.db"};
    WorkerChannel wchannel{wstore, 0, "127.0.0.1"};   // never start()ed
    hivemind::SecretStore secrets{tmp / "secrets.db",
                                  tmp / "secrets.key"};
    HubRouter hub{agents, threads, wchannel, approvals, routines,
                  secrets};
    hivemind::DeliveryStore deliveries{tmp / "deliveries.db"};
    ChatService chat{agents, threads, hub, deliveries};

    const std::string wkey = wstore.enroll("testrig");
    (void)wkey;
    auto alpha = agents.create_agent("Alpha", "", "", "", false, "",
                                     "testrig");
    auto beta  = agents.create_agent("Beta",  "", "", "", false, "",
                                     "testrig");

    // ── 65a: cancel_agent_turns mutes the agent ────────────────────────
    const std::string tid =
        chat.post_agent_message(alpha.id, beta.id, "first");
    assert(dispatch_failures(threads, tid) == 1);   // fan-in attempted

    const int cancelled = chat.cancel_agent_turns(beta.id);
    assert(cancelled == 0);   // no live turns (offline dispatch), but…

    chat.post_agent_message(alpha.id, beta.id, "second");
    assert(dispatch_failures(threads, tid) == 1);   // muted: NO dispatch

    // …the OTHER direction is not muted
    chat.post_agent_message(beta.id, alpha.id, "reply");
    assert(dispatch_failures(threads, tid) == 2);

    // user message lifts the mute
    chat.post_user_message(tid, "hello team");
    chat.post_agent_message(alpha.id, beta.id, "third");
    assert(dispatch_failures(threads, tid) >= 3);

    // ── 65b: circuit breaker trips and resets ──────────────────────────
    // Fresh pair/thread so the mute test's history doesn't interfere.
    // Breaker: max 3 fan-ins per window (user fan-ins are generation 0
    // and do not count).
    auto gamma   = agents.create_agent("Gamma", "", "", "", false, "",
                                       "testrig");
    auto delta   = agents.create_agent("Delta", "", "", "", false, "",
                                       "testrig");
    const std::string t2 =
        chat.post_agent_message(gamma.id, delta.id, "m1");
    chat.post_agent_message(gamma.id, delta.id, "m2");
    chat.post_agent_message(gamma.id, delta.id, "m3");
    assert(dispatch_failures(threads, t2) == 3);
    assert(breaker_notices(threads, t2) == 0);

    chat.post_agent_message(gamma.id, delta.id, "m4");   // trips
    assert(dispatch_failures(threads, t2) == 3);         // dropped
    assert(breaker_notices(threads, t2) == 1);           // announced once

    chat.post_agent_message(gamma.id, delta.id, "m5");   // still open
    assert(dispatch_failures(threads, t2) == 3);
    assert(breaker_notices(threads, t2) == 1);           // no re-announce

    // user reset fans generation-0 to BOTH agents (+2 dispatch attempts)
    // and lifts the breaker…
    chat.post_user_message(t2, "reset");
    assert(dispatch_failures(threads, t2) == 5);
    chat.post_agent_message(gamma.id, delta.id, "m6");
    assert(dispatch_failures(threads, t2) == 6);         // fan-in resumed

    std::cout << "test_chat_throttle: all assertions passed\n";
    return 0;
}
