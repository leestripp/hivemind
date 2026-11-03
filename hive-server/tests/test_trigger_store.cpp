#include "hivemind/TriggerStore.h"

#include <cassert>
#include <filesystem>
#include <iostream>
#include <thread>

namespace fs = std::filesystem;

int main() {
    const fs::path tmp = fs::temp_directory_path() / "hivemind_test_trigger_store";
    fs::remove_all(tmp);
    fs::create_directories(tmp);
    const fs::path db = tmp / "triggers.db";

    TriggerStore store{db};
    auto first = store.create("watcher-a", "thread-a", "agent_idle",
                              "target", "Continue when it is idle");
    auto second = store.create("watcher-b", "thread-b", "agent_idle",
                               "target", "");
    auto other = store.create("watcher-a", "thread-c", "agent_idle",
                              "other-target", "");

    assert(!first.id.empty());
    assert(first.agent_id == "watcher-a");
    assert(first.target_agent_id == "target");
    assert(!first.claimed);
    assert(store.get(first.id)->prompt == "Continue when it is idle");
    assert(store.list().size() == 3);
    assert(store.list("watcher-a").size() == 2);
    assert(store.list("watcher-b").size() == 1);
    assert(store.list("nobody").empty());

    const auto json = trigger_json(first);
    assert(json.at("event") == "agent_idle");
    assert(json.at("state") == "pending");
    assert(json.at("target_agent_id") == "target");

    // Atomic claim: one event target only, and a second caller cannot
    // receive the same one-shot rows.
    auto claimed = store.claim("agent_idle", "target");
    assert(claimed.size() == 2);
    assert(claimed[0].claimed && claimed[1].claimed);
    assert(store.claim("agent_idle", "target").empty());
    assert(store.get(first.id)->claimed);
    assert(!store.get(other.id)->claimed);
    assert(!store.complete(other.id)); // completion requires a claim

    assert(store.release(first.id));
    assert(!store.release(first.id));
    auto retry = store.claim("agent_idle", "target");
    assert(retry.size() == 1 && retry[0].id == first.id);
    assert(store.complete(first.id));
    assert(!store.get(first.id));
    assert(!store.complete(first.id));

    // Interrupted claims are recovered by opening the store, not lost.
    assert(store.get(second.id)->claimed);
    {
        TriggerStore recovered{db};
        assert(!recovered.get(second.id)->claimed);
        auto after_restart = recovered.claim("agent_idle", "target");
        assert(after_restart.size() == 1);
        assert(after_restart[0].id == second.id);
        assert(recovered.complete(second.id));
    }

    // Concurrent claimers partition the work instead of both receiving it.
    auto race = store.create("watcher-a", "thread-race", "agent_idle",
                             "race-target", "");
    TriggerStore claimant_b{db};
    std::vector<EventTrigger> a_rows;
    std::vector<EventTrigger> b_rows;
    std::thread a([&] { a_rows = store.claim("agent_idle", "race-target"); });
    std::thread b([&] { b_rows = claimant_b.claim("agent_idle", "race-target"); });
    a.join();
    b.join();
    assert(a_rows.size() + b_rows.size() == 1);
    assert(store.claim("agent_idle", "race-target").empty());

    // Background activity is durable across hub restart, zero clears it,
    // and roster deletion also removes a stale non-zero activity row.
    store.set_background_active("target", 2);
    assert(store.background_active("target") == 2);
    {
        TriggerStore recovered{db};
        assert(recovered.background_active("target") == 2);
        recovered.set_background_active("target", 0);
        assert(recovered.background_active("target") == 0);
        recovered.set_background_active("race-target", 1);
    }
    assert(store.background_active("race-target") == 1);

    // Roster deletion removes both owned subscriptions and watches of the
    // deleted agent, without touching unrelated rows.
    assert(store.remove_for_agent("race-target") == 1);
    assert(!store.get(race.id));
    assert(store.background_active("race-target") == 0);
    assert(store.get(other.id));
    assert(store.remove_for_agent("watcher-a") == 1);
    assert(store.list().empty());

    fs::remove_all(tmp);
    std::cout << "test_trigger_store: OK\n";
    return 0;
}
