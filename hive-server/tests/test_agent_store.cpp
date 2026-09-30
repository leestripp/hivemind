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

    // ── worker placement (Phase 30) ───────────────────────────────────────
    assert(a.worker == "local");   // default when not specified
    auto w = store.create_agent("Rigger", "gpu jobs", "", "shared", false, "",
                                "rig-gpu");
    assert(w.worker == "rig-gpu");
    assert(store.get_agent(w.id)->worker == "rig-gpu");   // persisted

    // ── get / list ────────────────────────────────────────────────────────
    auto b = store.create_agent("Pixel", "coder");
    assert(store.list_agents().size() == 3);
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

    // update_agent worker: "" keeps current, explicit value replaces
    auto moved = store.update_agent(w.id, "Rigger", "gpu jobs", "", "shared",
                                    false, "", "" /*keep*/);
    assert(moved.worker == "rig-gpu");
    moved = store.update_agent(w.id, "Rigger", "gpu jobs", "", "shared",
                               false, "", "local");
    assert(moved.worker == "local");

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
        assert(store2.list_agents().size() == 3);
        auto again = store2.get_agent(a.id);
        assert(again && again->role == "ops lead");
    }

    // ── type (Phase 35) ───────────────────────────────────────────────────
    auto g = store.create_agent("Forge", "3d artist", "", "shared", false, "",
                                "", "gpu");
    assert(g.type == "gpu");
    assert(store.get_agent(g.id)->type == "gpu");    // persisted
    assert(store.get_agent(a.id)->type.empty());     // default: standard
    // update with "" keeps the current type
    store.update_agent(g.id, "Forge", "3d artist", "", "shared", false);
    assert(store.get_agent(g.id)->type == "gpu");
    // "default" explicitly clears back to standard
    store.update_agent(g.id, "Forge", "3d artist", "", "shared", false, "",
                       "", "", "default");
    assert(store.get_agent(g.id)->type.empty());
    // ...and can be set again
    store.update_agent(g.id, "Forge", "3d artist", "", "shared", false, "",
                       "", "", "gpu");
    assert(store.get_agent(g.id)->type == "gpu");

    // ── memory / cpus (Phase 69) ──────────────────────────────────────────
    // create with explicit caps; defaults are ""/0 (worker decides).
    auto big = store.create_agent("Comfy", "image gen", "", "shared", false,
                                  "", "", "", "32g", 12);
    assert(big.memory == "32g");
    assert(big.cpus == 12);
    auto persisted = store.get_agent(big.id);
    assert(persisted && persisted->memory == "32g" && persisted->cpus == 12);
    assert(store.get_agent(a.id)->memory.empty());        // default unset
    assert(store.get_agent(a.id)->cpus == 0);
    // fractional cpus survive the REAL roundtrip
    store.update_agent(big.id, "Comfy", "image gen", "", "shared", false, "",
                       "", "", "", "", 1.5);
    assert(store.get_agent(big.id)->cpus == 1.5);
    assert(store.get_agent(big.id)->memory == "32g");     // "" = keep
    // cpus = -1 keeps, 0 resets to the worker default
    store.update_agent(big.id, "Comfy", "image gen", "", "shared", false, "",
                       "", "", "", "", -1);
    assert(store.get_agent(big.id)->cpus == 1.5);
    store.update_agent(big.id, "Comfy", "image gen", "", "shared", false, "",
                       "", "", "", "", 0);
    assert(store.get_agent(big.id)->cpus == 0);
    // memory "default" explicitly clears
    store.update_agent(big.id, "Comfy", "image gen", "", "shared", false, "",
                       "", "", "", "default", -1);
    assert(store.get_agent(big.id)->memory.empty());
    // ...and both can be set again via update
    store.update_agent(big.id, "Comfy", "image gen", "", "shared", false, "",
                       "", "", "", "1536m", 8);
    {
        auto r = store.get_agent(big.id);
        assert(r && r->memory == "1536m" && r->cpus == 8);
    }

    // ── delete ────────────────────────────────────────────────────────────
    assert(store.delete_agent(b.id));
    assert(!store.delete_agent(b.id));  // already gone
    assert(store.list_agents().size() == 4);   // Nova, Rigger, Forge, Comfy

    fs::remove_all(tmp);
    std::cout << "test_agent_store: OK\n";
    return 0;
}
