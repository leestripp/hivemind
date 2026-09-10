// tests/test_thread_nudges.cpp — proactive nudge query: threads whose
// latest message is an old unanswered user message.
#include "hivemind/ThreadStore.h"

#include <cassert>
#include <filesystem>
#include <iostream>

namespace fs = std::filesystem;

// Messages store UTC ISO strings; the query compares lexicographically.
// Fixed strings keep this test independent of wall time.
static const char* OLD = "2020-01-01T00:00:00.000000Z";   // ancient
static const char* NEW = "2999-01-01T00:00:00.000000Z";   // far future
static const char* THRESHOLD = "2026-01-01T00:00:00Z";

static bool has(const std::vector<Thread>& v, const std::string& id) {
    for (const auto& t : v) if (t.id == id) return true;
    return false;
}

int main() {
    fs::path tmp = fs::temp_directory_path() / "hivemind_test_thread_nudges";
    fs::remove_all(tmp);
    fs::create_directories(tmp);
    ThreadStore store{tmp / "threads.db"};

    // add_message stamps NOW; to control created_at we insert via a second
    // store handle... simpler: use add_message then fix created_at via the
    // SQL escape hatch the store doesn't expose → instead create messages
    // with add_message and pick thresholds around them. BUT the cleanest
    // is direct SQL through a raw sqlite open of the same file.
    auto t_old_user = store.create_thread("old unanswered", "dm", {"a1"});
    auto t_answered = store.create_thread("answered", "dm", {"a1"});
    auto t_system   = store.create_thread("nudged already", "dm", {"a1"});
    auto t_fresh    = store.create_thread("fresh", "dm", {"a1"});
    auto t_nopart   = store.create_thread("no agents", "dm", {});

    store.add_message(t_old_user.id, "user", "", "hello?");
    store.add_message(t_answered.id, "user", "", "hi");
    store.add_message(t_answered.id, "agent", "a1", "hello!");
    store.add_message(t_system.id, "user", "", "you there?");
    store.add_message(t_system.id, "system", "hivemind", "[nudge] ...");
    store.add_message(t_fresh.id, "user", "", "just now");
    store.add_message(t_nopart.id, "user", "", "echo...");

    // age the specific messages via raw SQL on the same db file
    {
        sqlite3* db = nullptr;
        assert(sqlite3_open((tmp / "threads.db").c_str(), &db) == SQLITE_OK);
        auto age_last = [&](const std::string& tid, const char* ts) {
            std::string sql =
                "UPDATE messages SET created_at = ? WHERE rowid ="
                " (SELECT MAX(rowid) FROM messages WHERE thread_id = ?)";
            sqlite3_stmt* st = nullptr;
            assert(sqlite3_prepare_v2(db, sql.c_str(), -1, &st, nullptr)
                   == SQLITE_OK);
            sqlite3_bind_text(st, 1, ts, -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(st, 2, tid.c_str(), -1, SQLITE_TRANSIENT);
            assert(sqlite3_step(st) == SQLITE_DONE);
            sqlite3_finalize(st);
        };
        age_last(t_old_user.id, OLD);   // old user msg
        age_last(t_nopart.id, OLD);     // old, but no participants
        sqlite3_close(db);
    }

    auto awaiting = store.threads_awaiting_agent(THRESHOLD);

    assert(has(awaiting, t_old_user.id));        // old unanswered user msg
    assert(!has(awaiting, t_answered.id));       // agent replied last
    assert(!has(awaiting, t_system.id));         // nudge already posted
    assert(!has(awaiting, t_fresh.id));          // too recent
    assert(!has(awaiting, t_nopart.id));         // no agent participants

    // participants are loaded for the hits
    for (const auto& t : awaiting)
        if (t.id == t_old_user.id)
            assert(t.agent_ids.size() == 1 && t.agent_ids[0] == "a1");

    // threshold later than the fresh message catches it too
    auto all = store.threads_awaiting_agent(NEW);
    assert(has(all, t_fresh.id));

    fs::remove_all(tmp);
    std::cout << "test_thread_nudges OK\n";
    return 0;
}
