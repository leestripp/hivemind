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

    // 2026-10-01: user_posted_thread_ids — the unread-badge gate. ONE
    // DISTINCT query; a thread counts only when it has a user message.
    auto posted = store.user_posted_thread_ids();
    assert(posted.count(dm.id) == 1);     // user messaged the DM
    assert(posted.count(grp.id) == 1);    // user messaged the group
    auto agent_only = store.create_thread("a2a relay", "group",
                                          {"agent-1", "agent-2"});
    store.add_message(agent_only.id, "agent", "agent-1", "relay ping");
    auto posted2 = store.user_posted_thread_ids();
    assert(posted2.count(agent_only.id) == 0);   // pure agent-to-agent
    assert(posted2.count(dm.id) == 1);           // unchanged
    assert(store.delete_thread(agent_only.id));  // keep later counts exact

    assert(store.list_messages(dm.id).size() == 3);
    assert(store.list_messages(grp.id).size() == 1);

    // ── persistence across instances ──────────────────────────────────────
    {
        ThreadStore store2{tmp / "threads.db"};
        assert(store2.list_messages(dm.id).size() == 3);
        auto t2 = store2.get_thread(grp.id);
        assert(t2 && t2->agent_ids.size() == 2);
    }

    // ── Phase 18: attachments round-trip ─────────────────────────────────
    const std::string atts = R"([{"name":"r.pdf","path":"/workspace/attachments/)"
        + dm.id + R"(/r.pdf","host_path":"/tmp/ws/attachments/)"
        + dm.id + R"(/r.pdf","size":1234}])";
    auto m4 = store.add_message(dm.id, "user", "", "see attached", atts);
    auto with_att = store.list_messages(dm.id);
    assert(with_att.size() == 4);
    assert(with_att[3].attachments_json == atts);
    // default is "[]" and survives the after_id path too
    assert(with_att[0].attachments_json == "[]");
    auto tail = store.list_messages(dm.id, m3);
    assert(tail.size() == 1 && tail[0].id == m4);
    assert(tail[0].attachments_json == atts);

    // ── delete cascades ───────────────────────────────────────────────────
    assert(store.delete_thread(dm.id));
    assert(!store.get_thread(dm.id));
    assert(store.list_messages(dm.id).empty());
    assert(!store.delete_thread(dm.id));
    assert(store.list_threads().size() == 2);



    // ── §MsgStatus: lifecycle markers ────────────────────────────────────
    {
        auto tid = store.create_thread("status suite", "group", {"a", "b"});
        const std::string mid =
            store.add_message(tid.id, "user", "", "task: rotate the keys");

        // Fresh messages carry no status on any read path.
        auto fresh = store.get_message(mid);
        assert(fresh && fresh->status.empty() && fresh->status_by.empty());
        assert(store.list_messages(tid.id).back().status.empty());
        bool has_more = true;
        auto win = store.list_messages_window(tid.id, "", 10, has_more);
        assert(!has_more && win.back().status.empty());

        // Set -> visible on every read path, at stamped, note kept.
        store.set_message_status(tid.id, mid, "claimed", "QueenBee",
                                 "taking this");
        auto cur = store.get_message(mid);
        assert(cur->status == "claimed" && cur->status_by == "QueenBee");
        assert(!cur->status_at.empty() && cur->status_note == "taking this");
        assert(store.list_messages(tid.id).back().status == "claimed");
        win = store.list_messages_window(tid.id, "", 10, has_more);
        assert(win.back().status == "claimed" &&
               win.back().status_by == "QueenBee");

        // Forward transition re-stamps by/note.
        store.set_message_status(tid.id, mid, "done", "QueenBee", "");
        cur = store.get_message(mid);
        assert(cur->status == "done" && cur->status_note.empty());

        store.set_message_status(tid.id, mid, "verified", "Forge",
                                 "port answers 200");
        cur = store.get_message(mid);
        assert(cur->status == "verified" && cur->status_by == "Forge");

        // Clear empties all four fields.
        store.set_message_status(tid.id, mid, "", "", "");
        cur = store.get_message(mid);
        assert(cur->status.empty() && cur->status_by.empty() &&
               cur->status_at.empty() && cur->status_note.empty());

        // Unknown message -> invalid_argument; wrong thread -> same.
        bool threw = false;
        try { store.set_message_status(tid.id, "no-such", "done", "x", ""); }
        catch (const std::invalid_argument&) { threw = true; }
        assert(threw);
        threw = false;
        try { store.set_message_status("no-thread", mid, "done", "x", ""); }
        catch (const std::invalid_argument&) { threw = true; }
        assert(threw);

        // Status on one message never leaks into another.
        const std::string mid2 =
            store.add_message(tid.id, "agent", "a", "unrelated");
        store.set_message_status(tid.id, mid2, "done", "a", "");
        assert(store.get_message(mid)->status.empty());
    }

    fs::remove_all(tmp);
    std::cout << "test_thread_store: OK\n";
    return 0;
}
