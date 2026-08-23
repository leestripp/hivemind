// tests/test_routine_store.cpp — RoutineStore CRUD + due-scan, no LLM.
#include "hivemind/RoutineStore.h"

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

    fs::remove_all(tmp);
    std::cout << "test_routine_store: OK\n";
    return 0;
}
