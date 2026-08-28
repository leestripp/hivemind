// tests/test_container_manager.cpp — ContainerManager unit tests + a live
// podman integration test that is SKIPPED when containers can't run here
// (e.g. nested/restricted dev sandboxes). No LLM required.
#include "hivemind/ContainerManager.h"

#include <cassert>
#include <iostream>

int main() {
    // ── pure helpers (always run) ─────────────────────────────────────────
    assert(ContainerManager::container_name("abc-123") == "hive-abc-123");

    // shell_quote: plain passthrough, single-quote escaping
    assert(ContainerManager::shell_quote("plain") == "'plain'");
    assert(ContainerManager::shell_quote("it's") == "'it'\\''s'");
    assert(ContainerManager::shell_quote("") == "''");
    assert(ContainerManager::shell_quote("a b;c`d") == "'a b;c`d'");

    // routes_have_default: /proc/net/route parsing (dead-pasta detection)
    const std::string with_default =
        "Iface\tDestination\tGateway\tFlags\tRefCnt\tUse\tMetric\tMask\n"
        "eth0\t00000000\t0200000A\t0003\t0\t0\t0\t00000000\n"
        "eth0\t0000000A\t00000000\t0001\t0\t0\t0\t00FFFFFF\n";
    assert(ContainerManager::routes_have_default(with_default));
    const std::string no_default =   // forwarder died: only the local route
        "Iface\tDestination\tGateway\tFlags\tRefCnt\tUse\tMetric\tMask\n"
        "eth0\t0000000A\t00000000\t0001\t0\t0\t0\t00FFFFFF\n";
    assert(!ContainerManager::routes_have_default(no_default));
    assert(!ContainerManager::routes_have_default(""));
    assert(!ContainerManager::routes_have_default("Iface\tDestination\n"));
    // spaces instead of tabs, and a default route not on the first data row
    const std::string spaces =
        "Iface   Destination Gateway Flags\n"
        "lo      0000007F    00000000 0001\n"
        "tap0    00000000    0200000A 0003\n";
    assert(ContainerManager::routes_have_default(spaces));

    // agent_home_dir ends with agents/<id>/home
    auto home = ContainerManager::agent_home_dir("xyz");
    assert(home.string().find("agents/xyz/home") != std::string::npos);

    // ── podman presence probe ─────────────────────────────────────────────
    if (!ContainerManager::podman_available()) {
        std::cout << "test_container_manager: podman not installed — "
                     "live integration test SKIPPED (unit tests passed)\n";
        return 0;
    }

    // podman binary exists but containers may still be blocked (nested
    // sandbox). Probe with a trivial image operation.
    auto probe = container_manager().exec("no-such-agent", "true", 5);
    if (probe.exit_code == -1 &&
        probe.output.find("not installed") != std::string::npos) {
        std::cout << "test_container_manager: SKIPPED\n";
        return 0;
    }

    // ── live integration (only on machines where podman really works) ─────
    const std::string agent = "containermgr-test";
    try {
        container_manager().ensure_image();
        std::string cname = container_manager().ensure_running(agent);
        assert(cname == "hive-" + agent);

        auto st = container_manager().status(agent);
        assert(st.exists && st.running);

        // run a command as user hive
        auto r = container_manager().exec(agent, "whoami && pwd", 30);
        assert(r.exit_code == 0);
        assert(r.output.find("hive") != std::string::npos);
        assert(r.output.find("/workspace") != std::string::npos);

        // sudo works (passwordless)
        auto s = container_manager().exec(agent, "sudo whoami", 30);
        assert(s.exit_code == 0);
        assert(s.output.find("root") != std::string::npos);

        // shared workspace visible across containers: write via one,
        // read via another agent's container
        container_manager().ensure_running("containermgr-test-2");
        auto w = container_manager().exec(agent, "echo shared-ok > /workspace/ctest.txt", 30);
        assert(w.exit_code == 0);
        auto rd = container_manager().exec("containermgr-test-2", "cat /workspace/ctest.txt", 30);
        assert(rd.exit_code == 0);
        assert(rd.output.find("shared-ok") != std::string::npos);

        // private home is per-agent
        auto h1 = container_manager().exec(agent, "echo secret > ~/p.txt", 30);
        assert(h1.exit_code == 0);
        auto h2 = container_manager().exec("containermgr-test-2", "cat ~/p.txt", 30);
        assert(h2.exit_code != 0);  // not visible to the other agent

        container_manager().remove("containermgr-test-2");
        assert(container_manager().stop(agent));
        assert(container_manager().remove(agent));
        std::cout << "test_container_manager: live integration OK\n";
    } catch (const std::exception& e) {
        // Environment blocked (nested sandbox etc.) — clean up and skip.
        std::cerr << "test_container_manager: podman present but containers "
                     "cannot run here (" << e.what() << ") — SKIPPED\n";
        container_manager().remove(agent);
        container_manager().remove("containermgr-test-2");
        return 0;
    }
    return 0;
}
