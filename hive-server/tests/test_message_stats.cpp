// tests/test_message_stats.cpp — Phase 64: per-turn stats round-trip
// through the messages table (stats_json column + SELECT/INSERT paths).
#include "hivemind/ThreadStore.h"

#include <cassert>
#include <filesystem>
#include <iostream>

namespace fs = std::filesystem;

int main() {
    fs::path tmp = fs::temp_directory_path() / "hivemind_test_message_stats";
    fs::remove_all(tmp);
    fs::create_directories(tmp);
    ThreadStore store{tmp / "threads.db"};

    auto t = store.create_thread("stats", "dm", {"a1"});

    // Message WITH stats round-trips byte-identical.
    const std::string stats =
        R"({"total_tokens":41234,"cached_tokens":38000,"iterations":3})";
    store.add_message(t.id, "agent", "a1", "reply with stats", "[]", stats);
    // Message WITHOUT stats gets the empty default.
    store.add_message(t.id, "user", "", "no stats here");

    auto msgs = store.list_messages(t.id, "");
    assert(msgs.size() == 2);
    assert(msgs[0].stats_json == stats);
    assert(msgs[1].stats_json.empty());

    // The windowed path (initial load / page-back) must read it too.
    bool has_more = false;
    auto win = store.list_messages_window(t.id, "", 10, has_more);
    assert(win.size() == 2);
    assert(win[0].stats_json == stats);

    fs::remove_all(tmp);
    std::cout << "test_message_stats: OK\n";
    return 0;
}
