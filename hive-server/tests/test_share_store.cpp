// test_share_store (2026-10-11, Plan.md "Agent-to-Agent Secure File
// Sharing"): ShareStore CRUD, addressed-share listing, fetch counters,
// revoke state machine, input validation.

#include "hivemind/ShareStore.h"

#include <cassert>
#include <cstdio>
#include <filesystem>
#include <stdexcept>

using hivemind::Share;
using hivemind::ShareStore;

namespace fs = std::filesystem;

static Share make_share(const std::string& id,
                        const std::string& sender,
                        const std::string& target) {
    Share s;
    s.id              = id;
    s.sender_agent_id = sender;
    s.target_agent_id = target;
    s.filename        = "report.pdf";
    s.size_bytes      = 12345;
    s.note            = "quarterly numbers";
    s.worker          = "ubuntu-pc";
    s.rel_path        = "shared/" + id + "/report.pdf";
    return s;
}

int main() {
    const fs::path dir = fs::temp_directory_path() / "hm_test_shares";
    fs::remove_all(dir);
    fs::create_directories(dir);
    ShareStore store(dir / "shares.db");

    // ── create + get ─────────────────────────────────────────────────
    store.create_share(make_share("sh-1", "agent-a", "agent-b"));
    auto s = store.get_share("sh-1");
    assert(s);
    assert(s->sender_agent_id == "agent-a");
    assert(s->target_agent_id == "agent-b");
    assert(s->size_bytes == 12345);
    assert(s->state == "active");
    assert(!s->created_at.empty());
    assert(s->fetched_at.empty());
    assert(s->fetch_count == 0);
    assert(!store.get_share("nope"));

    // ── validation ───────────────────────────────────────────────────
    {
        Share bad = make_share("", "agent-a", "agent-b");   // no id
        bool threw = false;
        try { store.create_share(bad); }
        catch (const std::invalid_argument&) { threw = true; }
        assert(threw);
    }
    {
        Share bad = make_share("sh-bad", "agent-a", "");    // no target
        bool threw = false;
        try { store.create_share(bad); }
        catch (const std::invalid_argument&) { threw = true; }
        assert(threw);
    }
    {
        Share bad = make_share("sh-neg", "agent-a", "agent-b");
        bad.size_bytes = -1;
        bool threw = false;
        try { store.create_share(bad); }
        catch (const std::invalid_argument&) { threw = true; }
        assert(threw);
    }

    // ── list_for_agent: both directions, isolation ───────────────────
    store.create_share(make_share("sh-2", "agent-b", "agent-a"));
    store.create_share(make_share("sh-3", "agent-c", "agent-d"));
    {
        const auto mine = store.list_for_agent("agent-a");
        assert(mine.size() == 2);            // sent sh-1, received sh-2
        for (const auto& row : mine)
            assert(row.sender_agent_id == "agent-a" ||
                   row.target_agent_id == "agent-a");
        const auto other = store.list_for_agent("agent-c");
        assert(other.size() == 1);
        assert(other[0].id == "sh-3");
        const auto all = store.list_for_agent("");   // admin audit
        assert(all.size() == 3);
        const auto capped = store.list_for_agent("", 2);
        assert(capped.size() == 2);
    }

    // ── mark_fetched: counters, state unchanged ──────────────────────
    store.mark_fetched("sh-1");
    store.mark_fetched("sh-1");
    s = store.get_share("sh-1");
    assert(s->fetch_count == 2);
    assert(!s->fetched_at.empty());
    assert(s->state == "active");

    // ── revoke: active -> revoked, idempotent ────────────────────────
    assert(store.revoke("sh-1"));
    s = store.get_share("sh-1");
    assert(s->state == "revoked");
    assert(!store.revoke("sh-1"));          // already revoked
    assert(!store.revoke("nope"));          // missing

    // Revoked shares still list (audit trail) with their state.
    {
        const auto mine = store.list_for_agent("agent-a");
        bool saw_revoked = false;
        for (const auto& row : mine)
            if (row.id == "sh-1") saw_revoked = (row.state == "revoked");
        assert(saw_revoked);
    }

    fs::remove_all(dir);
    std::puts("test_share_store: OK");
    return 0;
}
