#include "hivemind/AgentStore.h"
#include "hivemind/ChatService.h"
#include "hivemind/DeliveryStore.h"
#include "hivemind/HubRouter.h"
#include "hivemind/MessageHeader.h"
#include "hivemind/RoutineStore.h"
#include "hivemind/SecretStore.h"
#include "hivemind/ThreadStore.h"
#include "hivemind/TriggerStore.h"
#include "hivemind/WorkerChannel.h"
#include "hivemind/WorkerStore.h"

#include <cassert>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using nlohmann::json;

namespace {
struct Dispatch {
    std::string agent_id;
    std::string thread_id;
    std::string task_id;
    std::string prompt;
};

class MockHub final : public HubRouter {
public:
    using HubRouter::HubRouter;
    std::vector<Dispatch> dispatches;

    std::string dispatch(const RosterAgent& agent,
                         const std::string& thread_id,
                         const std::string& text) override {
        const std::string task = "trigger-task-" + std::to_string(next_++);
        dispatches.push_back({agent.id, thread_id, task, text});
        return task;
    }

    const Dispatch* last_for(const std::string& agent_id) const {
        const Dispatch* hit = nullptr;
        for (const auto& dispatch : dispatches)
            if (dispatch.agent_id == agent_id) hit = &dispatch;
        return hit;
    }

private:
    int next_ = 1;
};
} // namespace

int main() {
    setenv("HIVEMIND_TURN_TIMEOUT_S", "0", 1);
    const fs::path tmp = fs::temp_directory_path() / "hivemind_test_event_triggers";
    fs::remove_all(tmp);
    fs::create_directories(tmp);

    AgentStore agents{tmp / "agents.db"};
    ThreadStore threads{tmp / "threads.db"};
    RoutineStore routines{tmp / "routines.db"};
    TriggerStore triggers{tmp / "triggers.db"};
    WorkerStore workers{tmp / "workers.db"};
    WorkerChannel channel{workers, 0, "127.0.0.1"};
    hivemind::SecretStore secrets{tmp / "secrets.db", tmp / "secrets.key"};
    MockHub hub{agents, threads, channel, routines, secrets};
    hivemind::DeliveryStore deliveries{tmp / "deliveries.db"};
    ChatService chat{agents, threads, hub, deliveries};
    hub.set_chat_service(&chat);
    hub.set_trigger_store(&triggers);
    chat.set_trigger_store(&triggers);

    (void)workers.enroll("testrig");
    auto watcher = agents.create_agent("Watcher", "", "", "", "", "testrig");
    auto target = agents.create_agent("Target", "", "", "", "", "testrig");
    auto target_dm = threads.create_thread("Target", "dm", {target.id});

    // A trigger created during a live turn must wait. A second user message
    // keeps the target's queue non-empty, so completion of turn one is NOT
    // the idle event even though one LiveTurn has just retired.
    chat.post_user_message(target_dm.id, "first assignment");
    const std::string first_task = hub.last_for(target.id)->task_id;
    auto added = trigger_request_apply(triggers, agents, threads, &chat,
        watcher.id, {{"action", "add"}, {"event", "agent_idle"},
                     {"target", "Target"}, {"prompt", "Continue the handoff"}});
    assert(added.value("ok", false));
    const auto& created = added.at("result");
    const std::string trigger_id = created.at("id");
    const std::string trigger_thread = created.at("thread_id");
    assert(created.at("target_agent_id") == target.id);
    assert(!created.at("fired_immediately"));
    assert(triggers.get(trigger_id));
    assert(hub.last_for(watcher.id) == nullptr);

    chat.post_user_message(target_dm.id, "second assignment");
    chat.complete_turn(first_task, "first done", "");
    assert(hub.last_for(watcher.id) == nullptr);
    assert(triggers.get(trigger_id));
    const std::string second_task = hub.last_for(target.id)->task_id;
    assert(second_task != first_task);

    // A live background process keeps the target non-idle even after its
    // foreground turn ends. Process exit alone is not enough: the normal
    // background_done nudge and its resulting turn must finish first.
    hub.on_worker_frame("testrig", {{"op", "background_state"},
        {"agent", target.id}, {"active", 1}});
    assert(triggers.background_active(target.id) == 1);
    hub.on_worker_frame("testrig", {{"op", "task_result"},
        {"agent", target.id}, {"task", second_task},
        {"thread_id", target_dm.id},
        {"body", "foreground done, background running"}, {"error", ""},
        {"background_active", 1}});
    assert(hub.last_for(watcher.id) == nullptr);
    assert(triggers.get(trigger_id));

    hub.on_worker_frame("testrig", {{"op", "background_done"},
        {"agent", target.id}, {"thread_id", target_dm.id},
        {"process_id", "bg-test"}, {"active", 0}, {"exit_code", 0}});
    assert(triggers.background_active(target.id) == 0);
    const std::string background_task = hub.last_for(target.id)->task_id;
    assert(background_task != second_task);
    assert(hub.last_for(watcher.id) == nullptr);
    assert(triggers.get(trigger_id));
    chat.complete_turn(background_task, "background result handled", "");

    const auto* fired = hub.last_for(watcher.id);
    assert(fired);
    assert(fired->thread_id == trigger_thread);
    assert(fired->prompt.rfind("[trigger:agent_idle @ ", 0) == 0);
    assert(fired->prompt.find("Trigger: " + trigger_id) != std::string::npos);
    assert(fired->prompt.find("Watched agent: Target (" + target.id + ")")
           != std::string::npos);
    assert(fired->prompt.find("State: idle") != std::string::npos);
    assert(fired->prompt.find("Instructions:\nContinue the handoff")
           != std::string::npos);
    assert(!triggers.get(trigger_id)); // one-shot auto-removal
    const std::string first_fire_task = fired->task_id;

    const auto messages = threads.list_messages(trigger_thread);
    assert(messages.size() == 1);
    assert(messages[0].sender_kind == "user");
    assert(messages[0].body.rfind("[trigger:", 0) != 0); // raw stored body
    bool structured_source = false;
    for (const auto& delivery : deliveries.list(watcher.id, "", 20))
        if (delivery.source_message_id == messages[0].id) {
            structured_source =
                delivery.source == hivemind::msg_source::kAgentIdleTrigger;
        }
    assert(structured_source);

    // A later idle evaluation cannot re-fire the consumed registration.
    const std::size_t dispatch_count = hub.dispatches.size();
    chat.evaluate_agent_idle(target.id);
    assert(hub.dispatches.size() == dispatch_count);

    // Registration on an already-idle target fires synchronously through the
    // same path, closing the "finished just before subscribe" race.
    auto immediate = trigger_request_apply(triggers, agents, threads, &chat,
        watcher.id, {{"action", "add"}, {"event", "agent_idle"},
                     {"target", target.id}});
    assert(immediate.value("ok", false));
    assert(immediate.at("result").at("fired_immediately"));
    assert(immediate.at("result").at("state") == "fired");
    assert(!triggers.get(immediate.at("result").at("id")));
    // The watcher is still handling the first event, so this second event's
    // durable wake queues and drains on the watcher's own idle transition.
    chat.complete_turn(first_fire_task, "first trigger handled", "");
    assert(hub.last_for(watcher.id)->task_id != first_fire_task);
    assert(hub.last_for(watcher.id)->prompt.rfind(
        "[trigger:agent_idle @ ", 0) == 0);

    // Bridge validation and ownership. Keep Target busy so cancellation is
    // testable without an immediate fire.
    chat.post_user_message(target_dm.id, "hold for cancellation");
    const std::string cancel_task = hub.last_for(target.id)->task_id;
    auto pending = trigger_request_apply(triggers, agents, threads, &chat,
        watcher.id, {{"action", "add"}, {"event", "agent_idle"},
                     {"target", target.id}, {"prompt", "cancel me"}});
    const std::string pending_id = pending.at("result").at("id");
    const std::string pending_thread = pending.at("result").at("thread_id");

    auto watcher_list = trigger_request_apply(triggers, agents, threads, &chat,
        watcher.id, {{"action", "list"}});
    assert(watcher_list.at("result").at("triggers").size() == 1);
    const auto& row = watcher_list.at("result").at("triggers")[0];
    assert(!row.contains("prompt"));
    assert(row.at("prompt_chars") == 9);
    assert(row.at("target_agent_name") == "Target");
    auto target_list = trigger_request_apply(triggers, agents, threads, &chat,
        target.id, {{"action", "list"}});
    assert(target_list.at("result").at("triggers").empty());

    auto denied = trigger_request_apply(triggers, agents, threads, &chat,
        target.id, {{"action", "delete"}, {"trigger_id", pending_id}});
    assert(!denied.value("ok", true));
    assert(triggers.get(pending_id));
    auto removed = trigger_request_apply(triggers, agents, threads, &chat,
        watcher.id, {{"action", "delete"}, {"trigger_id", pending_id}});
    assert(removed.value("ok", false));
    assert(!triggers.get(pending_id));
    assert(!threads.get_thread(pending_thread));

    const std::size_t before_cancel_completion = hub.dispatches.size();
    chat.complete_turn(cancel_task, "cancelled watch done", "");
    assert(hub.dispatches.size() == before_cancel_completion);

    assert(!trigger_request_apply(triggers, agents, threads, &chat,
        watcher.id, {{"action", "add"}, {"event", "file_created"},
                     {"target", target.id}}).value("ok", true));
    assert(!trigger_request_apply(triggers, agents, threads, &chat,
        watcher.id, {{"action", "add"}, {"event", "agent_idle"},
                     {"target", watcher.id}}).value("ok", true));
    assert(!trigger_request_apply(triggers, agents, threads, &chat,
        watcher.id, {{"action", "add"}, {"event", "agent_idle"},
                     {"target", "missing"}}).value("ok", true));
    assert(!trigger_request_apply(triggers, agents, threads, &chat,
        watcher.id, {{"action", "add"}, {"event", "agent_idle"},
                     {"target", target.id},
                     {"prompt", std::string(4001, 'x')}}).value("ok", true));

    assert(hivemind::is_valid_message_source(
        hivemind::msg_source::kAgentIdleTrigger));
    assert(!hivemind::is_valid_message_source("trigger:other"));

    chat.quiesce();
    fs::remove_all(tmp);
    std::cout << "test_event_triggers: OK\n";
    return 0;
}
