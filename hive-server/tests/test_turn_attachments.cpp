// tests/test_turn_attachments.cpp — 2026-10-15 (Plan.md): attach_file
// attachments post IMMEDIATELY as their own message mid-turn so they
// survive turn interrupts (watchdog/cancel) — previously they only rode
// task_result and died with the turn (Bob's 15s video-ad incident).
// Covers: immediate post mid-turn, no double-post at complete_turn,
// unknown-task posting (hub-restart recovery), empty-attachment no-op,
// no fan-in from a mid-turn attachment.
#include "hivemind/ChatService.h"
#include "hivemind/HubRouter.h"
#include "hivemind/AgentStore.h"
#include "hivemind/ThreadStore.h"
#include "hivemind/DeliveryStore.h"
#include "hivemind/WorkerChannel.h"
#include "hivemind/WorkerStore.h"
#include "hivemind/RoutineStore.h"
#include "hivemind/SecretStore.h"

#include <cassert>
#include <filesystem>
#include <iostream>

namespace fs = std::filesystem;

namespace {

class MockHub final : public HubRouter {
public:
    using HubRouter::HubRouter;
    struct Dispatch { std::string agent_id, thread_id, task_id; };
    std::vector<Dispatch> dispatches;
    int next = 0;
    std::string dispatch(const RosterAgent& agent,
                         const std::string& thread_id,
                         const std::string& /*text*/) override {
        std::string tid = "mock-task-" + std::to_string(++next);
        dispatches.push_back({agent.id, thread_id, tid});
        return tid;
    }
};

const char* kAtts = R"json([{"name":"ad.mp4",
    "rel_path":"attachments/T/ad.mp4",
    "path":"/workspace/attachments/T/ad.mp4","size":12345,
    "worker":"testrig"}])json";

int count_agent_msgs(ThreadStore& threads, const std::string& tid) {
    int n = 0;
    for (const auto& m : threads.list_messages(tid))
        if (m.sender_kind == "agent") ++n;
    return n;
}

} // namespace

int main() {
    fs::path tmp =
        fs::temp_directory_path() / "hivemind_test_turn_attachments";
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

    (void)wstore.enroll("testrig");
    auto bob   = agents.create_agent("Bob",   "", "", "", "",
                                     "testrig");
    auto honey = agents.create_agent("Honey", "", "", "", "",
                                     "testrig");
    auto dm = threads.create_thread("Bob", "dm", {bob.id});

    // 1) Turn goes live, then an attachment arrives MID-TURN.
    chat.post_user_message(dm.id, "render me a 15s ad");
    assert(hub.dispatches.size() == 1);
    const std::string task = hub.dispatches[0].task_id;
    assert(count_agent_msgs(threads, dm.id) == 0);

    chat.post_turn_attachment(task, dm.id, bob.id, kAtts);
    {
        const auto msgs = threads.list_messages(dm.id);
        int att_msgs = 0;
        for (const auto& m : msgs)
            if (m.sender_kind == "agent" &&
                m.attachments_json.find("ad.mp4") != std::string::npos)
                ++att_msgs;
        assert(att_msgs == 1 &&
               "attachment must post immediately, mid-turn");
        assert(count_agent_msgs(threads, dm.id) == 1);
    }

    // 2) Turn completes with a text body and NO attachments (they were
    //    already shipped) — the reply posts separately, no double-post.
    chat.complete_turn(task, "here's your ad!", "", "[]");
    assert(count_agent_msgs(threads, dm.id) == 2);
    {
        int att_msgs = 0;
        for (const auto& m : threads.list_messages(dm.id))
            if (m.attachments_json.find("ad.mp4") != std::string::npos)
                ++att_msgs;
        assert(att_msgs == 1 && "attachment must not double-post");
    }

    // 3) Unknown task (hub restart wiped LiveTurns) still posts — the
    //    frame carries thread/agent, same recovery rule as complete_turn.
    chat.post_turn_attachment("dead-task", dm.id, bob.id, kAtts);
    assert(count_agent_msgs(threads, dm.id) == 3);

    // 4) Empty / missing inputs are silent no-ops.
    chat.post_turn_attachment("x", dm.id, bob.id, "[]");
    chat.post_turn_attachment("x", dm.id, bob.id, "");
    chat.post_turn_attachment("x", "", bob.id, kAtts);
    chat.post_turn_attachment("x", dm.id, "", kAtts);
    chat.post_turn_attachment("x", "no-such-thread", bob.id, kAtts);
    assert(count_agent_msgs(threads, dm.id) == 3);

    // 5) A mid-turn attachment is NOT a turn boundary: it must not fan
    //    in to peer agents. Post into a Bob+Honey thread and assert
    //    Honey is never tasked.
    auto pair = threads.create_thread("Bob & Honey", "group",
                                      {bob.id, honey.id});
    const size_t disp_before = hub.dispatches.size();
    chat.post_turn_attachment("t", pair.id, bob.id, kAtts);
    assert(count_agent_msgs(threads, pair.id) == 1);
    for (size_t i = disp_before; i < hub.dispatches.size(); ++i)
        assert(hub.dispatches[i].agent_id != honey.id &&
               "mid-turn attachment must not fan in");

    std::cout << "test_turn_attachments: all assertions passed\n";
    return 0;
}
