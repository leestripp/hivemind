// tests/test_thread_store.cpp — ThreadStore threads/messages, no LLM required.
#include "hivemind/ThreadStore.h"

#include <cassert>
#include <filesystem>
#include <iostream>

namespace fs = std::filesystem;

int main() {
    fs::path tmp = fs::temp_directory_path() / "hivemind_test_thread_store";
    fs::remove_all(tmp);
    fs::create_directories(tmp);

    ThreadStore store{tmp / "threads.db"};

    // ── create dm + group ─────────────────────────────────────────────────
    auto dm = store.create_thread("chat with Nova", "dm", {"agent-1"});
    assert(!dm.id.empty());
    assert(dm.kind == "dm");
    assert(dm.agent_ids.size() == 1 && dm.agent_ids[0] == "agent-1");

    auto grp = store.create_thread("standup", "group", {"agent-1", "agent-2"});
    assert(grp.kind == "group");
    assert(grp.agent_ids.size() == 2);

    // two agents with kind "dm" auto-promotes to group
    auto auto_grp = store.create_thread("", "dm", {"a", "b"});
    assert(auto_grp.kind == "group");

    // ── get / list ────────────────────────────────────────────────────────
    auto got = store.get_thread(dm.id);
    assert(got && got->title == "chat with Nova");
    assert(got->agent_ids.size() == 1);
    assert(!store.get_thread("no-such-id"));
    assert(store.list_threads().size() == 3);

    // ── messages ──────────────────────────────────────────────────────────
    auto m1 = store.add_message(dm.id, "user", "", "hello Nova");
    auto m2 = store.add_message(dm.id, "agent", "agent-1", "hi! how can I help?");
    auto m3 = store.add_message(dm.id, "user", "", "summarize my notes");

    auto all = store.list_messages(dm.id);
    assert(all.size() == 3);
    assert(all[0].body == "hello Nova");
    assert(all[1].sender_kind == "agent" && all[1].sender_id == "agent-1");
    assert(all[2].body == "summarize my notes");

    // incremental fetch: after m1 → only m2, m3
    auto rest = store.list_messages(dm.id, m1);
    assert(rest.size() == 2);
    assert(rest[0].id == m2);
    assert(rest[1].id == m3);

    // after last message → empty
    assert(store.list_messages(dm.id, m3).empty());

    // messages of other threads are isolated
    store.add_message(grp.id, "user", "", "group hello");
    assert(store.list_messages(dm.id).size() == 3);
    assert(store.list_messages(grp.id).size() == 1);

    // ── persistence across instances ──────────────────────────────────────
    {
        ThreadStore store2{tmp / "threads.db"};
        assert(store2.list_messages(dm.id).size() == 3);
        auto t2 = store2.get_thread(grp.id);
        assert(t2 && t2->agent_ids.size() == 2);
    }

    // ── delete cascades ───────────────────────────────────────────────────
    assert(store.delete_thread(dm.id));
    assert(!store.get_thread(dm.id));
    assert(store.list_messages(dm.id).empty());
    assert(!store.delete_thread(dm.id));
    assert(store.list_threads().size() == 2);

    fs::remove_all(tmp);
    std::cout << "test_thread_store: OK\n";
    return 0;
}
