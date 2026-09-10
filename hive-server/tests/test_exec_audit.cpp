// tests/test_exec_audit.cpp — Phase 8 exec audit trail store.
#include "hivemind/ExecAuditStore.h"

#include <cassert>
#include <filesystem>
#include <iostream>

namespace fs = std::filesystem;

int main() {
    fs::path tmp = fs::temp_directory_path() / "hivemind_test_exec_audit";
    fs::remove_all(tmp);
    fs::create_directories(tmp);
    ExecAuditStore store{tmp / "exec_audit.db"};

    store.log("agent-1", "ls -la /workspace", 0, false, 42);
    store.log("agent-1", "python3 browser_driver.py goto 'https://x'", 1, false, 1500);
    store.log("agent-2", "sudo apt install foo", 0, false, 9000);
    store.log("agent-1", "sleep 999", 124, true, 60000);

    // per-agent, newest first
    auto a1 = store.list("agent-1");
    assert(a1.size() == 3);
    assert(a1[0].command == "sleep 999");            // newest first
    assert(a1[0].timed_out && a1[0].exit_code == 124);
    assert(a1[0].duration_ms == 60000);
    assert(a1[2].command == "ls -la /workspace");
    for (const auto& e : a1) {
        assert(e.agent_id == "agent-1");
        assert(!e.ts.empty());
        assert(e.id > 0);
    }

    // all agents
    auto all = store.list("");
    assert(all.size() == 4);

    // limit respected
    assert(store.list("agent-1", 2).size() == 2);

    // long commands truncated to 500 chars + ellipsis
    std::string big(2000, 'x');
    store.log("agent-1", big, 0, false, 1);
    auto top = store.list("agent-1", 1);
    assert(top[0].command.size() == ExecAuditStore::kCommandLimit + 3); // "…" is 3 bytes UTF-8
    assert(top[0].command.ends_with("…"));

    // persistence across store instances (same file)
    {
        ExecAuditStore store2{tmp / "exec_audit.db"};
        assert(store2.list("").size() == 5);
    }

    fs::remove_all(tmp);
    std::cout << "test_exec_audit OK\n";
    return 0;
}
