// tests/test_thread_groups.cpp — Phase 67 user thread groups (folders).
#include "hivemind/ThreadStore.h"

#include <cassert>
#include <filesystem>
#include <iostream>
#include <stdexcept>

namespace fs = std::filesystem;

int main() {
    fs::path tmp = fs::temp_directory_path() / "hivemind_test_thread_groups";
    fs::remove_all(tmp);
    fs::create_directories(tmp);

    ThreadStore store{tmp / "threads.db"};
    auto t1 = store.create_thread("dm one", "dm", {"agent-1"});
    auto t2 = store.create_thread("pair", "group", {"agent-1", "agent-2"});
    auto t3 = store.create_thread("routine x", "routine", {"agent-3"});

    // ── CRUD ─────────────────────────────────────────────────────────
    const std::string g1 = store.create_group("ExplorOz");
    assert(!g1.empty());
    const std::string g2 = store.create_group("Research");
    auto groups = store.list_groups();
    assert(groups.size() == 2);
    assert(groups[0].name == "ExplorOz" && groups[0].position == 0);
    assert(groups[1].name == "Research" && groups[1].position == 1);
    assert(groups[0].member_count == 0);

    store.rename_group(g1, "ExplorOz Project");
    assert(store.list_groups()[0].name == "ExplorOz Project");
    bool threw = false;
    try { store.rename_group("no-such-group", "x"); }
    catch (const std::invalid_argument&) { threw = true; }
    assert(threw);
    threw = false;
    try { store.create_group(""); }
    catch (const std::invalid_argument&) { threw = true; }
    assert(threw);

    // ── membership: move, folder rule, smart fallback ────────────────
    store.set_thread_group(t1.id, g1);
    store.set_thread_group(t2.id, g1);
    {
        auto m = store.thread_group_map();
        assert(m.size() == 2 && m[t1.id] == g1 && m[t2.id] == g1);
        assert(store.list_groups()[0].member_count == 2);
    }
    // folder rule: re-moving t2 to g2 replaces, never duplicates
    store.set_thread_group(t2.id, g2);
    {
        auto m = store.thread_group_map();
        assert(m.size() == 2 && m[t2.id] == g2);
    }
    // smart fallback: empty group_id drops the row
    store.set_thread_group(t2.id, "");
    {
        auto m = store.thread_group_map();
        assert(m.size() == 1 && m.count(t2.id) == 0);
    }
    // unknown thread / group are errors
    threw = false;
    try { store.set_thread_group("no-such-thread", g1); }
    catch (const std::invalid_argument&) { threw = true; }
    assert(threw);
    threw = false;
    try { store.set_thread_group(t1.id, "no-such-group"); }
    catch (const std::invalid_argument&) { threw = true; }
    assert(threw);

    // ── delete group reverts members to smart ────────────────────────
    store.set_thread_group(t3.id, g1);
    store.delete_group(g1);
    {
        auto m = store.thread_group_map();
        assert(m.empty());   // t1 and t3 reverted
        assert(store.list_groups().size() == 1);
    }
    threw = false;
    try { store.delete_group(g1); }   // already gone
    catch (const std::invalid_argument&) { threw = true; }
    assert(threw);

    // ── persistence across reopen ────────────────────────────────────
    store.set_thread_group(t1.id, g2);
    {
        ThreadStore store2{tmp / "threads.db"};
        auto m = store2.thread_group_map();
        assert(m.size() == 1 && m[t1.id] == g2);
        auto gs = store2.list_groups();
        assert(gs.size() == 1 && gs[0].name == "Research"
               && gs[0].member_count == 1);
    }

    fs::remove_all(tmp);
    std::cout << "test_thread_groups: OK\n";
    return 0;
}
