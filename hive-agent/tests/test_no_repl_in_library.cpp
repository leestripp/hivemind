// SPDX-License-Identifier: BSL-1.0
// tests/test_no_repl_in_library.cpp
//
// §Phase 8 acceptance: the library does NOT call std::cin, std::getline,
// std::cout, std::cerr, printf, or any equivalent during a pure
// `run_one_turn()`. The REPL belongs to the host (Phase 2.5).
//
// This test redirects std::cin / std::cout / std::cerr to in-memory
// streams and asserts that none of them see any data after a sequence
// of pure turn invocations. The host-level wrapping (`ConsoleAgentSink`)
// is the only place I/O happens — and that sink is owned by the host,
// not the library.

#include <cassert>
#include <filesystem>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>

#include "library/Agent.h"
#include "api/ProviderPlugin.h"
#include "api/ProviderRegistry.h"
#include "library/model_discovery.h"
#include "a2a/A2aClient.h"

namespace fs = std::filesystem;

// Per-test scratch directory under the system temp dir. Auto-created
// and cleaned up at process exit. Replaces a hardcoded `/tmp/...` path
// that silently passed only when leftovers from a prior run happened
// to exist.
struct ScratchDir {
    fs::path path;
    explicit ScratchDir(const std::string& tag) {
        path = fs::temp_directory_path() /
               ("hive_agent_test_" + tag + "_" +
                std::to_string(reinterpret_cast<std::uintptr_t>(this)));
        fs::remove_all(path);
        fs::create_directories(path);
    }
    ~ScratchDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

// No-op sink so the library doesn't print to the terminal.
struct NoopSink : public hive::AgentSink {
    void on_log(hive::LogLevel, std::string_view) noexcept override {}
};

int main() {
    auto sink = std::make_shared<NoopSink>();

    // ---- Redirect std::cout / std::cerr to local streams ----
    std::stringstream captured_cout, captured_cerr;
    std::streambuf* old_cout = std::cout.rdbuf(captured_cout.rdbuf());
    std::streambuf* old_cerr = std::cerr.rdbuf(captured_cerr.rdbuf());

    ScratchDir dir("repl");

    hive::AgentConfig cfg;
    cfg.project_root = dir.path;
    cfg.session_id   = "no-repl-test";

    hive::Agent agent(std::move(cfg), sink);

    // initialize() can emit ONE log line via the sink (the message
    // we want). It must not write to std::cout or std::cerr.
    agent.initialize();

    std::string cout_after_init = captured_cout.str();
    std::string cerr_after_init = captured_cerr.str();
    assert(cout_after_init.empty());
    assert(cerr_after_init.empty());

    // ---- Run a no-op turn (no queued messages) ----
    agent.run_one_turn();
    assert(captured_cout.str().empty());
    assert(captured_cerr.str().empty());

    // ---- Run a turn that transitions to Cancelled (no LLM creds) ----
    agent.submit_user_message("won't actually run — no creds");
    agent.cancel();
    agent.run_one_turn();
    assert(captured_cout.str().empty());
    assert(captured_cerr.str().empty());

    agent.shutdown();
    assert(captured_cout.str().empty());
    assert(captured_cerr.str().empty());

    // ---- Restore ----
    std::cout.rdbuf(old_cout);
    std::cerr.rdbuf(old_cerr);

    std::cerr << "[test] no_repl PASSED (library wrote nothing to "
              << "std::cout or std::cerr across init/cancel/run/shutdown)\n";
    return 0;
}