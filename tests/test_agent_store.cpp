// tests/test_agent_store.cpp — AgentStore (roster) CRUD, no LLM required.
#include "hivemind/AgentStore.h"

#include <cassert>
#include <filesystem>
#include <iostream>
#include <stdexcept>

namespace fs = std::filesystem;

int main() {
    fs::path tmp = fs::temp_directory_path() / "hivemind_test_agent_store";
    fs::remove_all(tmp);
    fs::create_directories(tmp);

    AgentStore store{tmp / "agents.db"};

    // ── create ────────────────────────────────────────────────────────────
    auto a = store.create_agent("Nova", "research assistant", "Kimi-K2.7");
    assert(!a.id.empty());
    assert(a.name == "Nova");
    assert(a.role == "research assistant");
    assert(a.model == "Kimi-K2.7");
    assert(a.workspace_mode == "shared");
    assert(a.container_id.empty());
    assert(!a.created_at.empty());

    // ── duplicate name rejected ───────────────────────────────────────────
    bool threw = false;
    try { store.create_agent("Nova"); }
    catch (const std::invalid_argument&) { threw = true; }
    assert(threw);

    // ── empty name rejected ───────────────────────────────────────────────
    threw = false;
    try { store.create_agent(""); }
    catch (const std::invalid_argument&) { threw = true; }
    assert(threw);

    // ── get / list ────────────────────────────────────────────────────────
    auto b = store.create_agent("Pixel", "coder");
    assert(store.list_agents().size() == 2);
    auto got = store.get_agent(a.id);
    assert(got && got->name == "Nova");
    auto by_name = store.get_agent_by_name("Pixel");
    assert(by_name && by_name->id == b.id);
    assert(!store.get_agent("no-such-id"));

    // ── update ────────────────────────────────────────────────────────────
    auto updated = store.update_agent(a.id, "Nova", "ops lead", "", "shared", true);
    assert(updated.role == "ops lead");
    assert(updated.model.empty());
    assert(updated.updated_at >= a.created_at);
    assert(updated.require_approval == true);

    // name clash on update rejected
    threw = false;
    try { store.update_agent(a.id, "Pixel", "", "", "", false); }
    catch (const std::invalid_argument&) { threw = true; }
    assert(threw);

    // updating a missing agent rejected
    threw = false;
    try { store.update_agent("no-such-id", "X", "", "", "", false); }
    catch (const std::invalid_argument&) { threw = true; }
    assert(threw);

    // ── persistence across instances (same db path) ───────────────────────
    {
        AgentStore store2{tmp / "agents.db"};
        assert(store2.list_agents().size() == 2);
        auto again = store2.get_agent(a.id);
        assert(again && again->role == "ops lead");
    }

    // ── delete ────────────────────────────────────────────────────────────
    assert(store.delete_agent(b.id));
    assert(!store.delete_agent(b.id));  // already gone
    assert(store.list_agents().size() == 1);

    fs::remove_all(tmp);
    std::cout << "test_agent_store: OK\n";
    return 0;
}
