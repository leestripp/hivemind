// =============================================================================
// test_migration_store — Phase 73 (Plan.md). Job CRUD, the per-agent live
// concurrency gate, state transitions, and the sweeper query.
// =============================================================================

#include <cassert>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <stdexcept>

#include "hivemind/MigrationStore.h"

namespace fs = std::filesystem;
using hivemind::Migration;
using hivemind::MigrationStore;

namespace {

fs::path temp_db(const char* name) {
    auto dir = fs::temp_directory_path() / name;
    fs::remove_all(dir);
    fs::create_directories(dir);
    return dir / "migrations.db";
}

Migration sample(const std::string& agent = "agent-1") {
    Migration m;
    m.agent_id = agent;
    m.agent_name = "Buzz";
    m.source_worker = "ubuntu-pc";
    m.target_worker = "droplet";
    m.target_type = "container";
    return m;
}

}  // namespace

int main() {
    const auto db = temp_db("hivemind_test_migration_store");

    // ── create + get ────────────────────────────────────────────────
    MigrationStore store(db);
    const std::string id = store.create(sample());
    assert(!id.empty());
    auto got = store.get(id);
    assert(got.has_value());
    assert(got->state == "pending");
    assert(got->agent_name == "Buzz");
    assert(got->created_at == got->updated_at);
    assert(got->export_size == 0);

    // ── validation ──────────────────────────────────────────────────
    bool threw = false;
    try {
        Migration bad = sample("agent-2");
        bad.target_type = "vmware";
        store.create(bad);
    } catch (const std::invalid_argument&) { threw = true; }
    assert(threw);

    threw = false;
    try {
        store.create(sample(""));  // no agent
    } catch (const std::invalid_argument&) { threw = true; }
    assert(threw);

    // ── one live job per agent ──────────────────────────────────────
    threw = false;
    try {
        store.create(sample());  // same agent, still pending
    } catch (const std::invalid_argument&) { threw = true; }
    assert(threw);

    // ── state transitions + error/progress ──────────────────────────
    store.set_state(id, "exporting", "", "stopping agent");
    got = store.get(id);
    assert(got->state == "exporting");
    assert(got->progress == "stopping agent");
    assert(got->error.empty());
    assert(got->updated_at >= got->created_at);

    store.set_state(id, "failed", "source worker offline", "");
    got = store.get(id);
    assert(got->state == "failed");
    assert(got->error == "source worker offline");
    assert(got->progress == "stopping agent");  // preserved

    // ── terminal state releases the slot ────────────────────────────
    assert(!hivemind::MigrationStore::is_live("done"));
    assert(!hivemind::MigrationStore::is_live("failed"));
    assert(!hivemind::MigrationStore::is_live("cancelled"));
    assert(!hivemind::MigrationStore::is_live("rolled_back"));
    assert(hivemind::MigrationStore::is_live("importing"));

    const std::string id2 = store.create(sample());  // now allowed
    assert(id2 != id);

    // ── export metadata ─────────────────────────────────────────────
    store.set_export(id2, "/workspace/migrations/job/export.tar.zst", 424242);
    got = store.get(id2);
    assert(got->export_path == "/workspace/migrations/job/export.tar.zst");
    assert(got->export_size == 424242);

    // ── live_job_for / inflight / list ──────────────────────────────
    assert(store.live_job_for("agent-1").has_value());
    assert(!store.live_job_for("agent-2").has_value());

    store.set_state(id2, "verifying");
    auto live = store.inflight();
    assert(live.size() == 1);
    assert(live.front().id == id2);

    store.set_state(id2, "done");
    assert(store.inflight().empty());
    assert(!store.live_job_for("agent-1").has_value());

    // history keeps both, newest first
    const auto all = store.list("agent-1");
    assert(all.size() == 2);
    assert(all.front().id == id2);

    // ── persistence across reopen (durable) ─────────────────────────
    MigrationStore reopened(db);
    auto regot = reopened.get(id);
    assert(regot.has_value());
    assert(regot->state == "failed");
    assert(reopened.list().size() == 2);

    std::cout << "test_migration_store: OK\n";
    return 0;
}
