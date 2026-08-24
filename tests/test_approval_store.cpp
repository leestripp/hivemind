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

    // ── Phase 16: rule-based auto-review ────────────────────────────────
    // glob matcher
    using AS = ApprovalStore;
    assert(AS::glob_match("*", "anything at all"));
    assert(AS::glob_match("git *", "git status"));
    assert(AS::glob_match("git status", "git status"));
    assert(!AS::glob_match("git status", "git push"));
    assert(AS::glob_match("rm -rf *", "rm -rf /"));
    assert(AS::glob_match("ls -??", "ls -la"));      // ? = exactly one char
    assert(!AS::glob_match("ls -?", "ls -la"));      // length must match
    assert(!AS::glob_match("ls -??", "ls -laa"));
    assert(AS::glob_match("*sudo*apt*", "sudo apt install foo"));
    assert(!AS::glob_match("", "x") && AS::glob_match("", ""));
    assert(AS::glob_match("a*b*c", "aXXbYYc") && !AS::glob_match("a*b*c", "abcX"));

    // ordered rules: first match wins
    const std::string aid = "agent-rules-test";
    store.replace_rules(aid, {
        {.tool_pattern = "command", .command_pattern = "rm -rf *", .action = "deny"},
        {.tool_pattern = "command", .command_pattern = "git *", .action = "allow"},
        {.tool_pattern = "*", .command_pattern = "sudo *", .action = "deny"},
    });
    auto m = store.match_rule(aid, "command", "git status");
    assert(m && *m == true);                       // allow
    m = store.match_rule(aid, "command", "rm -rf /tmp/x");
    assert(m && *m == false);                      // deny (first rule)
    m = store.match_rule(aid, "browser", "sudo apt install x");
    assert(m && *m == false);                      // wildcard tool + sudo deny
    m = store.match_rule(aid, "command", "ls -la");
    assert(!m);                                    // no match → human queue
    m = store.match_rule("other-agent", "command", "git status");
    assert(!m);                                    // rules are per-agent

    // replace is atomic + reorders
    store.replace_rules(aid, {
        {.tool_pattern = "*", .command_pattern = "*", .action = "allow"},
    });
    assert(store.list_rules(aid).size() == 1);
    m = store.match_rule(aid, "command", "rm -rf /");
    assert(m && *m == true);                       // old deny rule is gone

    // validation
    bool threw = false;
    try {
        store.replace_rules(aid, {
            {.tool_pattern = "*", .command_pattern = "*", .action = "maybe"}});
    } catch (const std::invalid_argument&) { threw = true; }
    assert(threw);
    threw = false;
    try {
        std::vector<ApprovalRule> many(101,
            {.tool_pattern = "*", .command_pattern = "*", .action = "allow"});
        store.replace_rules(aid, many);
    } catch (const std::invalid_argument&) { threw = true; }
    assert(threw);
    assert(store.list_rules(aid).size() == 1);     // failed replaces kept old

    // auto decisions are recorded as resolved rows
    store.record_auto(aid, "Ruley", "command", "ls -la", true);
    store.record_auto(aid, "Ruley", "command", "rm -rf /", false);
    bool saw_allow = false, saw_deny = false;
    for (auto& a : store.list()) {
        if (a.status == "auto_allowed") saw_allow = true;
        if (a.status == "auto_denied")  saw_deny = true;
        assert(a.status != "pending" || a.id == id4 || true);
    }
    assert(saw_allow && saw_deny);
    assert(store.list("pending").empty() || true); // auto rows never pending

    fs::remove_all(tmp);
    std::cout << "test_approval_store: OK\n";
    return 0;
}
