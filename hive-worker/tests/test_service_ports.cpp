// test_service_ports.cpp — per-agent service-port registry + host
// address derivation (2026-10-09, Bob/SlateApe ingress feature).
//
// Covers hwrunner::service_port_base (allocate / persist / stable /
// lowest-free / disabled) and service_host_addr (env override).
// Container interaction is NOT tested here — that's the proven
// ensure_container path exercised live at deploy time.
//
// assert()-based like the rest of the worker suite; the macro keeps
// -UNDEBUG so these stay live in Release builds.

#include "../src/runner.h"

#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <cassert>

namespace fs = std::filesystem;

static void unset_all() {
    unsetenv("HIVE_SERVICE_POOL_BASE");
    unsetenv("HIVE_SERVICE_PORT_COUNT");
    unsetenv("HIVE_SERVICE_HOST");
}

int main() {
    const fs::path home = fs::temp_directory_path() / "hw-test-svcports";
    fs::remove_all(home);
    fs::create_directories(home / ".local" / "share" / "hive-worker");
    setenv("HOME", home.c_str(), 1);
    unset_all();

    // ── default allocation: first agent gets the pool base ──
    const int a = hwrunner::service_port_base("uuid-alice");
    assert(a == 18400);

    // ── second agent: one range further up ──
    const int b = hwrunner::service_port_base("uuid-bob");
    assert(b == 18410);

    // ── stability: repeat requests return the same base, registry persists ──
    assert(hwrunner::service_port_base("uuid-alice") == a);
    assert(hwrunner::service_port_base("uuid-bob") == b);
    const fs::path reg = home / ".local/share/hive-worker/service_ports.txt";
    assert(fs::exists(reg));
    {
        std::ifstream in(reg);
        std::string body((std::istreambuf_iterator<char>(in)),
                         std::istreambuf_iterator<char>());
        assert(body.find("uuid-alice 18400") != std::string::npos);
        assert(body.find("uuid-bob 18410") != std::string::npos);
    }

    // ── custom pool base + count are honored for NEW allocations ──
    setenv("HIVE_SERVICE_POOL_BASE", "29000", 1);
    setenv("HIVE_SERVICE_PORT_COUNT", "4", 1);
    const int c = hwrunner::service_port_base("uuid-carol");
    assert(c == 29000);
    assert(hwrunner::service_port_base("uuid-alice") == 18400);  // pinned

    // ── disabled feature returns 0 and never allocates ──
    setenv("HIVE_SERVICE_PORT_COUNT", "0", 1);
    assert(hwrunner::service_port_base("uuid-dave") == 0);

    // ── host address: env override wins; default is a usable IPv4 ──
    unset_all();
    setenv("HIVE_SERVICE_HOST", "services.example.internal", 1);
    assert(hwrunner::service_host_addr() == "services.example.internal");
    unsetenv("HIVE_SERVICE_HOST");
    const std::string addr = hwrunner::service_host_addr();
    assert(!addr.empty());
    assert(addr.find('.') != std::string::npos);   // dotted-quad shape

    fs::remove_all(home);
    std::cout << "test_service_ports: all assertions passed\n";
    return 0;
}
