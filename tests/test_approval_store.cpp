// tests/test_approval_store.cpp — ApprovalStore queue semantics, no LLM.
#include "hivemind/ApprovalStore.h"

#include <cassert>
#include <filesystem>
#include <iostream>
#include <thread>

namespace fs = std::filesystem;

int main() {
    fs::path tmp = fs::temp_directory_path() / "hivemind_test_approval_store";
    fs::remove_all(tmp);
    fs::create_directories(tmp);

    ApprovalStore store{tmp / "approvals.db"};

    // ── create + get ──────────────────────────────────────────────────────
    auto id = store.create("agent-1", "Nova", "command", "rm -rf /tmp/x");
    assert(!id.empty());
    auto a = store.get(id);
    assert(a && a->status == "pending");
    assert(a->agent_name == "Nova");
    assert(a->kind == "command");
    assert(a->payload == "rm -rf /tmp/x");
    assert(a->resolved_at.empty());

    // ── list filters ──────────────────────────────────────────────────────
    auto id2 = store.create("agent-2", "Pixel", "command", "sudo apt install jq");
    assert(store.list().size() == 2);
    assert(store.list("pending").size() == 2);
    assert(store.list("approved").empty());

    // ── resolve ───────────────────────────────────────────────────────────
    assert(store.resolve(id, true));
    assert(store.get(id)->status == "approved");
    assert(!store.get(id)->resolved_at.empty());
    // double resolve rejected
    assert(!store.resolve(id, false));
    // unknown id rejected
    assert(!store.resolve("no-such-id", true));
    assert(store.list("pending").size() == 1);

    // ── wait: approved path (resolver thread approves after 300ms) ────────
    std::thread resolver([&store, id2] {
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        store.resolve(id2, true);
    });
    bool granted = store.wait(id2, 10);
    resolver.join();
    assert(granted);
    assert(store.get(id2)->status == "approved");

    // ── wait: denied path ─────────────────────────────────────────────────
    auto id3 = store.create("agent-1", "Nova", "command", "drop table users");
    std::thread denier([&store, id3] {
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        store.resolve(id3, false);
    });
    bool granted3 = store.wait(id3, 10);
    denier.join();
    assert(!granted3);
    assert(store.get(id3)->status == "denied");

    // ── wait: timeout → expired ───────────────────────────────────────────
    auto id4 = store.create("agent-1", "Nova", "command", "never resolved");
    bool granted4 = store.wait(id4, 1);   // 1s timeout
    assert(!granted4);
    assert(store.get(id4)->status == "expired");

    fs::remove_all(tmp);
    std::cout << "test_approval_store: OK\n";
    return 0;
}
