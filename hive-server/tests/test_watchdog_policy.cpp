// 2026-11-11 Option 4: elapsed time is diagnostic, never a kill reason.
// Deterministic sweeps cover long productive turns, quiet warnings, repeated
// tool observation, progress recovery, explicit-cancel grace and completion.
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
#include <chrono>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace std::chrono_literals;

namespace {

class MockHub final : public HubRouter {
public:
    using HubRouter::HubRouter;
    struct Dispatch { std::string task_id; };
    std::vector<Dispatch> dispatches;
    int next = 0;

    std::string dispatch(const RosterAgent&, const std::string&,
                         const std::string&) override {
        const std::string id = "watchdog-task-" + std::to_string(++next);
        dispatches.push_back({id});
        return id;
    }
};

ChatService::LiveTurn live_for(ChatService& chat, const std::string& agent_id) {
    const auto activity = chat.agent_activity();
    const auto it = activity.find(agent_id);
    assert(it != activity.end());
    return it->second;
}

int system_notices(ThreadStore& threads, const std::string& thread_id,
                   const std::string& needle) {
    int count = 0;
    for (const auto& m : threads.list_messages(thread_id))
        if (m.sender_kind == "system" &&
            m.body.find(needle) != std::string::npos)
            ++count;
    return count;
}

bool agent_body_exists(ThreadStore& threads, const std::string& thread_id,
                       const std::string& needle) {
    for (const auto& m : threads.list_messages(thread_id))
        if (m.sender_kind == "agent" &&
            m.body.find(needle) != std::string::npos)
            return true;
    return false;
}

}  // namespace

int main() {
    setenv("HIVEMIND_STALL_NOTIFY_S", "300", 1);
    setenv("HIVEMIND_CANCEL_GRACE_S", "90", 1);

    const fs::path tmp = fs::temp_directory_path() /
        "hivemind_test_watchdog_policy";
    fs::remove_all(tmp);
    fs::create_directories(tmp);

    AgentStore agents{tmp / "agents.db"};
    ThreadStore threads{tmp / "threads.db"};
    RoutineStore routines{tmp / "routines.db"};
    WorkerStore workers{tmp / "workers.db"};
    WorkerChannel channel{workers, 0, "127.0.0.1"};
    hivemind::SecretStore secrets{tmp / "secrets.db", tmp / "secrets.key"};
    MockHub hub{agents, threads, channel, routines, secrets};
    hivemind::DeliveryStore deliveries{tmp / "deliveries.db"};
    ChatService chat{agents, threads, hub, deliveries};
    hub.set_chat_service(&chat);

    (void)workers.enroll("testrig");
    const auto forge = agents.create_agent("Forge", "", "", "", "", "testrig");
    const auto dm = threads.create_thread("Forge", "dm", {forge.id});

    // A long turn with repeated bash calls remains live past the old 1200s
    // kill point. Tool results between calls do not hide the repetition.
    chat.post_user_message(dm.id, "calculate for as long as necessary");
    assert(hub.dispatches.size() == 1);
    const std::string long_task = hub.dispatches[0].task_id;
    chat.note_progress(long_task, "tool_call", "execute_bash", "started");
    chat.note_progress(long_task, "tool_result", "execute_bash", "running");
    chat.note_progress(long_task, "tool_call", "execute_bash", "retrying");
    auto live = live_for(chat, forge.id);
    assert(live.last_tool == "execute_bash");
    assert(live.repeated_tool_calls == 2);
    assert(!live.stalled && !live.cancelling);

    chat.watchdog_sweep(live.last_activity + 299s);
    live = live_for(chat, forge.id);
    assert(!live.stalled && !live.cancelling);

    chat.watchdog_sweep(live.last_activity + 1201s);
    live = live_for(chat, forge.id);
    assert(live.stalled && !live.cancelling &&
           "age/quiet may warn but must never auto-cancel");
    auto rows = deliveries.find_all_by_task(long_task);
    assert(rows.size() == 1 && rows[0].state == "dispatched");
    assert(system_notices(threads, dm.id, "will NOT be cancelled") == 1);
    assert(system_notices(threads, dm.id, "2 consecutive calls") == 1);

    // One warning per quiet period, not a notification storm.
    chat.watchdog_sweep(live.last_activity + 1800s);
    assert(system_notices(threads, dm.id, "will NOT be cancelled") == 1);

    // Fresh tool progress clears the stall and arms a future quiet warning.
    chat.note_progress(long_task, "tool_progress", "execute_bash", "50%");
    live = live_for(chat, forge.id);
    assert(!live.stalled && live.activity_kind == "tool_progress");
    chat.append_delta(long_task, "still calculating");
    live = live_for(chat, forge.id);
    assert(live.activity_kind == "assistant_delta" && !live.stalled);
    chat.watchdog_sweep(live.last_activity + 301s);
    assert(system_notices(threads, dm.id, "will NOT be cancelled") == 2);
    assert(!live_for(chat, forge.id).cancelling);

    chat.complete_turn(long_task, "long computation completed", "");
    assert(agent_body_exists(threads, dm.id, "long computation completed"));
    rows = deliveries.find_all_by_task(long_task);
    assert(rows.size() == 1 && rows[0].state == "done");

    // Explicit Stop retains bounded recovery when the worker never answers.
    chat.post_user_message(dm.id, "start another job");
    assert(hub.dispatches.size() == 2);
    const std::string cancel_task = hub.dispatches[1].task_id;
    chat.cancel_thread(dm.id);
    live = live_for(chat, forge.id);
    assert(live.cancelling);
    chat.watchdog_sweep(live.cancel_started + 89s);
    assert(chat.busy(dm.id));
    chat.watchdog_sweep(live.cancel_started + 91s);
    assert(!chat.busy(dm.id));
    rows = deliveries.find_all_by_task(cancel_task);
    assert(rows.size() == 1 && rows[0].state == "error");
    assert(rows[0].error == "cancel unconfirmed (watchdog)");
    assert(system_notices(threads, dm.id, "cancel was never confirmed") == 1);

    // The late worker result remains tombstoned after explicit cancellation.
    chat.complete_turn(cancel_task, "late result must not appear", "");
    assert(!agent_body_exists(threads, dm.id, "late result must not appear"));

    // Completion before a later sweep leaves no stale timeout/stall notice.
    chat.post_user_message(dm.id, "finish cleanly");
    assert(hub.dispatches.size() == 3);
    const std::string final_task = hub.dispatches[2].task_id;
    live = live_for(chat, forge.id);
    chat.complete_turn(final_task, "clean result", "");
    chat.watchdog_sweep(live.last_activity + 3600s);
    assert(agent_body_exists(threads, dm.id, "clean result"));
    assert(system_notices(threads, dm.id, "will NOT be cancelled") == 2);
    rows = deliveries.find_all_by_task(final_task);
    assert(rows.size() == 1 && rows[0].state == "done");

    std::cout << "test_watchdog_policy: all assertions passed\n";
    return 0;
}
