// tests/test_user_inbox.cpp — user_inbox_snapshot(): the task-frame
// payload backing the agent's read_user_inbox tool. No LLM required.
#include "hivemind/HubRouter.h"
#include "hivemind/AgentStore.h"
#include "hivemind/ThreadStore.h"

#include <cassert>
#include <filesystem>
#include <iostream>

namespace fs = std::filesystem;

int main() {
    fs::path tmp = fs::temp_directory_path() / "hivemind_test_user_inbox";
    fs::remove_all(tmp);
    fs::create_directories(tmp);

    AgentStore agents{tmp / "agents.db"};
    ThreadStore threads{tmp / "threads.db"};

    auto pa = agents.create_agent("PA", "assistant", "");
    auto buzz = agents.create_agent("Buzz", "news", "");

    // Empty inbox → empty array, never an error.
    {
        auto snap = user_inbox_snapshot(threads, agents);
        assert(snap.is_array() && snap.empty());
    }

    threads.ensure_user_thread();
    threads.add_message(kUserThreadId, "agent", pa.id, "first note");
    threads.add_message(kUserThreadId, "agent", buzz.id, "second note");
    threads.add_message(kUserThreadId, "agent", "ghost-id", "sender unknown");

    // Chronological order; sender ids resolve to display names; unknown
    // senders fall back to their raw id rather than failing.
    {
        auto snap = user_inbox_snapshot(threads, agents);
        assert(snap.size() == 3);
        assert(snap[0]["sender"] == "PA");
        assert(snap[0]["text"] == "first note");
        assert(!snap[0]["created_at"].get<std::string>().empty());
        assert(snap[1]["sender"] == "Buzz");
        assert(snap[2]["sender"] == "ghost-id");
    }

    // Window cap: with limit 2 only the LATEST two come back, in order.
    {
        auto snap = user_inbox_snapshot(threads, agents, 2);
        assert(snap.size() == 2);
        assert(snap[0]["text"] == "second note");
        assert(snap[1]["text"] == "sender unknown");
    }

    fs::remove_all(tmp);
    std::cout << "test_user_inbox: OK\n";
    return 0;
}
