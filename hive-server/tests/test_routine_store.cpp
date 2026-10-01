// tests/test_routine_store.cpp — RoutineStore CRUD + due-scan, no LLM.
#include "hivemind/RoutineStore.h"
#include "hivemind/Schedule.h"

#include <cassert>
#include <filesystem>
#include <iostream>

namespace fs = std::filesystem;

int main() {
    fs::path tmp = fs::temp_directory_path() / "hivemind_test_routine_store";
    fs::remove_all(tmp);
    fs::create_directories(tmp);

    RoutineStore store{tmp / "routines.db"};

    auto r = store.create("agent-1", "thread-1", "morning-brief",
                          "Summarize overnight news",
                          R"({"type":"daily","time":"09:00"})",
                          "2026-08-24T09:00:00");
    assert(!r.id.empty());
    assert(r.enabled);
    assert(r.next_fire_at == "2026-08-24T09:00:00");
    assert(r.last_fired_at.empty());

    // due scan: not due yet
    assert(store.list_due("2026-08-23T10:00:00").empty());
    // due when now >= next_fire_at
    auto due = store.list_due("2026-08-24T09:00:01");
    assert(due.size() == 1 && due[0].id == r.id);

    // mark_fired reschedules
    store.mark_fired(r.id, "2026-08-24T09:00:01", "2026-08-25T09:00:00", false);
    auto after = store.get(r.id);
    assert(after->last_fired_at == "2026-08-24T09:00:01");
    assert(after->next_fire_at == "2026-08-25T09:00:00");
    assert(after->enabled);

    // mark_fired with disable (once-schedule exhausted)
    auto once = store.create("agent-1", "thread-2", "one-shot",
                             "Do it once", R"({"type":"once","at":"2026-08-24T10:00"})",
                             "2026-08-24T10:00:00");
    store.mark_fired(once.id, "2026-08-24T10:00:00", "", true);
    auto oc = store.get(once.id);
    assert(!oc->enabled);
    assert(oc->next_fire_at.empty());
    // disabled routine never comes due even with a past fire time
    assert(store.list_due("2027-01-01T00:00:00").size() == 1);  // only r

    // set_enabled toggle
    assert(store.set_enabled(r.id, false));
    assert(store.list_due("2027-01-01T00:00:00").empty());
    assert(store.set_enabled(r.id, true));

    // list + persistence across instances
    assert(store.list().size() == 2);
    {
        RoutineStore store2{tmp / "routines.db"};
        assert(store2.list().size() == 2);
        assert(store2.get(r.id)->prompt == "Summarize overnight news");
    }

    // remove
    assert(store.remove(once.id));
    assert(!store.remove(once.id));
    assert(store.list().size() == 1);

    // ── Phase 17: skip-if-running ────────────────────────────────────
    // Policy decisions (pure function):
    const json once_sch = json::parse(R"({"type":"once","at":"2026-08-24T10:00"})");
    const json daily_sch = json::parse(R"({"type":"daily","time":"09:00"})");
    using A = hm::RoutineFireAction;
    // idle + online fires
    assert(hm::routine_fire_action(once_sch,  false, true) == A::Fire);
    assert(hm::routine_fire_action(daily_sch, false, true) == A::Fire);
    // busy: recurring VOIDED, once retried (no queue growth either way)
    assert(hm::routine_fire_action(once_sch,  true,  true) == A::RetryLater);
    assert(hm::routine_fire_action(daily_sch, true,  true) == A::SkipReschedule);
    // OFFLINE (2026-10-01): recurring VOIDED — an offline fire would
    // stack a durable QUEUED delivery row per tick and drain them all
    // at once on reconnect; once retried per tick until back online.
    assert(hm::routine_fire_action(once_sch,  false, false) == A::RetryLater);
    assert(hm::routine_fire_action(daily_sch, false, false) == A::SkipReschedule);
    assert(hm::routine_fire_action(once_sch,  true,  false) == A::RetryLater);
    assert(hm::routine_fire_action(daily_sch, true,  false) == A::SkipReschedule);
    // invalid/absent type counts as recurring (never silently drop one-shot
    // semantics, but a busy invalid routine must not spin either)
    assert(hm::routine_fire_action(json::object(), true, true) == A::SkipReschedule);

    // reschedule advances next_fire_at WITHOUT touching last_fired_at
    auto busy = store.create("agent-1", "thread-busy", "busy-daily",
                             "tick", R"({"type":"daily","time":"09:00"})",
                             "2026-08-24T09:00:00");
    store.mark_fired(busy.id, "2026-08-24T09:00:01", "2026-08-25T09:00:00", false);
    store.reschedule(busy.id, "2026-08-26T09:00:00", false);
    auto b = store.get(busy.id);
    assert(b->last_fired_at == "2026-08-24T09:00:01");   // unchanged
    assert(b->next_fire_at == "2026-08-26T09:00:00");    // advanced
    assert(b->enabled);
    // skipped fire is no longer due at the old slot
    for (auto& d : store.list_due("2026-08-25T10:00:00"))
        assert(d.id != busy.id);
    // reschedule with disable (schedule exhausted while busy)
    store.reschedule(busy.id, "", true);
    b = store.get(busy.id);
    assert(!b->enabled && b->next_fire_at.empty());
    assert(b->last_fired_at == "2026-08-24T09:00:01");   // still unchanged

    fs::remove_all(tmp);
    std::cout << "test_routine_store: OK\n";
    return 0;
}
