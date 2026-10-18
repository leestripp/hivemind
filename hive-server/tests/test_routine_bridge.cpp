// tests/test_routine_bridge.cpp — agent routine-bridge helpers
// (RoutineStore.h): the fix for the 2026-10-18 Buzz routine-edit
// incident (Plan.md).
//
// Background: the agent's tool results are middle-truncated at 10KB.
// The bridge LIST used to embed every routine's FULL prompt, so one
// long-prompt routine pushed the list past the cap and the middle-cut
// swallowed the ids of everything after it — the agent could never
// edit what it could not see. Fixes under test:
//   1. routine_compact_json: the LIST shape — no prompt body, so the
//      list cannot blow the cap at any realistic routine count.
//   2. routine_full_json: the "get" shape — full prompt, one routine.
//   3. resolve_routine_ref: update/delete/get accept id OR exact name;
//      ambiguous names fail with the candidate ids (never guess);
//      ownership enforced for ids and names alike.
// No LLM, no WorkerChannel required.
#include "hivemind/RoutineStore.h"

#include <cassert>
#include <filesystem>
#include <iostream>
#include <string>

namespace fs = std::filesystem;
using nlohmann::json;

int main() {
    fs::path tmp = fs::temp_directory_path() / "hivemind_test_routine_bridge";
    fs::remove_all(tmp);
    fs::create_directories(tmp);

    RoutineStore store{tmp / "routines.db"};
    const std::string owner = "agent-buzz";
    const std::string other = "agent-bob";
    const std::string daily = R"({"type":"daily","time":"09:00"})";
    // Long enough that embedding it N times in a list would blow the
    // agent's 10KB tool-result cap on its own.
    const std::string long_prompt(12000, 'x');

    auto r1 = store.create(owner, "thread-1", "daily-news", long_prompt,
                           daily, "2026-10-19T09:00:00");
    auto r2 = store.create(owner, "thread-2", "social-warmup", "short",
                           daily, "2026-10-19T10:00:00");
    // Same NAME owned by a different agent — must never be reachable
    // through the owner's name fallback.
    auto r3 = store.create(other, "thread-3", "daily-news", "not yours",
                           daily, "2026-10-19T09:00:00");

    // --- 1. compact json: no prompt body, prompt_chars stands in. ---
    {
        const json c = routine_compact_json(r1);
        assert(!c.contains("prompt"));
        assert(c.value("prompt_chars", 0) == static_cast<int>(long_prompt.size()));
        assert(c.value("id", "") == r1.id);
        assert(c.value("name", "") == "daily-news");
        assert(c.contains("schedule"));
        assert(!c.value("schedule_text", "").empty());
        assert(c.contains("next_fire_at"));
        assert(c.contains("enabled"));
        assert(c.contains("created_at"));
        assert(c.contains("last_fired_at"));
        // The compact shape of BOTH routines together stays far under
        // the agent's 10KB tool-result cap — the actual invariant the
        // bug broke (Buzz's truncated list hid this routine's id).
        json arr = json::array({routine_compact_json(r1),
                                routine_compact_json(r2),
                                routine_compact_json(r3)});
        assert(arr.dump().size() < 10000);
    }

    // --- 2. full json: prompt present (the "get" shape). ---
    {
        const json f = routine_full_json(r1);
        assert(f.value("prompt", "") == long_prompt);
        assert(f.value("thread_id", "") == "thread-1");
        assert(f.value("agent_id", "") == owner);
    }

    // --- 3. resolve by id. ---
    {
        auto lk = resolve_routine_ref(store, owner, r1.id);
        assert(lk.routine && lk.error.empty());
        assert(lk.routine->id == r1.id);
    }

    // --- 4. resolve by exact name (the incident's fix). ---
    {
        auto lk = resolve_routine_ref(store, owner, "social-warmup");
        assert(lk.routine && lk.error.empty());
        assert(lk.routine->id == r2.id);
    }

    // --- 5. name fallback never crosses ownership. ---
    {
        // "daily-news" exists for both agents; owner resolves to HIS.
        auto lk = resolve_routine_ref(store, owner, "daily-news");
        assert(lk.routine && lk.routine->id == r1.id);
        // The other agent's id is refused for the owner...
        auto denied = resolve_routine_ref(store, owner, r3.id);
        assert(!denied.routine);
        assert(denied.error.find("belongs to another agent") != std::string::npos);
        // ...and the owner asking for a name HE does not own fails clean.
        auto none = resolve_routine_ref(store, "agent-nobody", "daily-news");
        assert(!none.routine);
        assert(none.error.find("not found") != std::string::npos);
    }

    // --- 6. unknown ref: not found, error names the ref. ---
    {
        auto lk = resolve_routine_ref(store, owner, "no-such-routine");
        assert(!lk.routine);
        assert(lk.error.find("no-such-routine") != std::string::npos);
    }

    // --- 7. empty ref rejected. ---
    {
        auto lk = resolve_routine_ref(store, owner, "");
        assert(!lk.routine);
        assert(!lk.error.empty());
    }

    // --- 8. ambiguous name: error lists BOTH ids, never guesses. ---
    {
        auto dup1 = store.create(owner, "thread-4", "dup", "a",
                                 daily, "2026-10-19T11:00:00");
        auto dup2 = store.create(owner, "thread-5", "dup", "b",
                                 daily, "2026-10-19T12:00:00");
        auto lk = resolve_routine_ref(store, owner, "dup");
        assert(!lk.routine);
        assert(lk.error.find("matches 2") != std::string::npos);
        assert(lk.error.find(dup1.id) != std::string::npos);
        assert(lk.error.find(dup2.id) != std::string::npos);
        // Ids still resolve fine despite the name collision.
        assert(resolve_routine_ref(store, owner, dup1.id).routine.has_value());
    }

    fs::remove_all(tmp);
    std::cout << "test_routine_bridge: OK\n";
    return 0;
}
