// SPDX-License-Identifier: BSL-1.0
// tests/test_cancel_stale_flag.cpp
//
// §BUG-005 regression: AgentImpl::cancel() used to latch
// cancel_requested_ unconditionally, and the flag was only cleared by
// the exchange() at the entry of the NEXT run_one_turn(). A cancel()
// with no work in flight (ESC at idle) therefore poisoned the next
// submitted turn, which aborted instantly with a spurious Cancelled.
//
// These tests pin the fixed contract:
//   - cancel() at idle (no turn, no queued messages) is a no-op for
//     future turns.
//   - cancel() with a queued message still aborts that turn
//     (long-standing behaviour, also covered by test_sink_events).
//
// Hermetic: cfg.agent_home points at a scratch dir, so the
// SettingsStore is empty, no LLMClient is created, and a dispatched
// turn lands in the Error branch — which is exactly how we prove the
// turn ran past the entry cancel check instead of being swallowed.

#include <filesystem>
#include <string>

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "library/Agent.h"
#include "api/ProviderPlugin.h"
#include "api/ProviderRegistry.h"
#include "library/model_discovery.h"
#include "a2a/A2aClient.h"

namespace fs = std::filesystem;

namespace {

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

hive::AgentConfig make_cfg(const fs::path& dir, const std::string& session) {
    hive::AgentConfig cfg;
    cfg.project_root  = dir.string();
    cfg.agent_home = dir.string();  // isolated, empty store → no LLMClient
    cfg.session_id    = session;
    return cfg;
}

}  // namespace

TEST_CASE("cancel at idle does not poison the next turn") {
    ScratchDir dir("cancel_idle");
    auto sink = std::make_shared<hive::AgentSink>();
    hive::Agent agent(make_cfg(dir.path, "cancel-idle"), sink);
    agent.initialize();

    // Bare cancel at idle — no turn in flight, nothing queued.
    agent.cancel();

    // The next submitted turn must run normally. With no LLMClient the
    // dispatch surfaces as Error; the bug made it abort as Cancelled.
    agent.submit_user_message("hello");
    agent.run_one_turn();
    CHECK(agent.state() == hive::AgentState::Error);

    agent.shutdown();
}

TEST_CASE("cancel with a queued message still aborts that turn") {
    ScratchDir dir("cancel_queued");
    auto sink = std::make_shared<hive::AgentSink>();
    hive::Agent agent(make_cfg(dir.path, "cancel-queued"), sink);
    agent.initialize();

    agent.submit_user_message("ignored — will be cancelled");
    agent.cancel();
    agent.run_one_turn();
    CHECK(agent.state() == hive::AgentState::Cancelled);

    // … and the cancel must be fully consumed: the following turn runs.
    agent.submit_user_message("hello again");
    agent.run_one_turn();
    CHECK(agent.state() == hive::AgentState::Error);

    agent.shutdown();
}
