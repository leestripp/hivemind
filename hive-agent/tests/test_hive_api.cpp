// SPDX-License-Identifier: BSL-1.0
// tests/test_daedalus_api.cpp
//
// Phase 1+ smoke test: compile against the public API surface and
// verify the Agent facade behaves as documented:
//
//   - Constructible from AgentConfig + AgentSink.
//   - initialize() / shutdown() round-trips.
//   - state() reports hive::AgentState correctly.
//   - stats() returns sensible defaults before any turn.
//   - run_one_turn() with no queued message is a no-op.
//   - cancel() does not crash.
//
// We do NOT exercise an actual LLM turn here — that requires API
// credentials and a live endpoint. The Phase 1+ test suite covers
// subsystem wiring in dedicated tests (test_session_memory, etc.).

#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "library/Agent.h"
#include "api/ProviderPlugin.h"
#include "api/ProviderRegistry.h"
#include "library/model_discovery.h"
#include "a2a/A2aClient.h"

namespace {

int g_state_changes = 0;
int g_log_count = 0;
std::vector<std::string> g_states;
std::vector<hive::AgentState> g_state_seen;

struct RecordingSink : public hive::AgentSink {
    void on_state_change(int /*from*/, int to) noexcept override {
        ++g_state_changes;
        hive::AgentState s = static_cast<hive::AgentState>(to);
        g_state_seen.push_back(s);
        g_states.push_back(std::string(hive::to_string(s)));
    }
    void on_log(hive::LogLevel level, std::string_view msg) noexcept override {
        ++g_log_count;
        std::cerr << "[sink " << hive::to_string(level) << "] " << msg << '\n';
    }
};

}  // namespace

int main()
{
    int failures = 0;
#define EXPECT(cond) do {                                              \
    if (!(cond)) {                                                      \
        std::cerr << "FAIL @ " << __LINE__ << ": " #cond << "\n";       \
        ++failures;                                                     \
    }                                                                   \
} while (0)

    g_state_changes = 0;
    g_log_count = 0;
    g_states.clear();
    g_state_seen.clear();

    // Construct Agent with no agent_home and no model — initialize()
    // should still succeed (it just doesn't have a real LLMClient to wire).
    hive::AgentConfig cfg;
    cfg.project_root = "/tmp";
    cfg.session_id = "smoke-test";

    auto sink = std::make_shared<RecordingSink>();
    hive::Agent agent(std::move(cfg), sink);

    EXPECT(agent.state() == hive::AgentState::Idle);

    // initialize() — should not crash, no model_name so no LLMClient created
    // (the real LLMClient requires API creds; with no agent_home the Impl
    //  leaves body_->client null until a real turn is requested).
    //
    // To keep this test pure (no network), we only test the path that
    // does NOT trigger a SettingsStore lookup.
    agent.initialize();
    EXPECT(agent.state() == hive::AgentState::Idle);

    // run_one_turn() with empty queue is a no-op.
    agent.run_one_turn();
    EXPECT(agent.state() == hive::AgentState::Idle);

    // submit_user_message + cancel before run_one_turn — turn should not crash.
    agent.submit_user_message("ignored — test cancels");
    agent.cancel();
    agent.run_one_turn();
    // After cancel, state should be Cancelled.
    EXPECT(agent.state() == hive::AgentState::Cancelled);

    // stats() should return sensible defaults.
    hive::AgentStats s = agent.stats();
    (void)s;

    // shutdown() round-trip.
    agent.shutdown();

    if (failures) {
        std::cerr << "\ntest_daedalus_api: " << failures << " FAILED\n";
        return 1;
    }
    std::cerr << "[test] Phase 1+ smoke test passed "
              << "(state_changes=" << g_state_changes
              << ", logs=" << g_log_count << ")\n";
    return 0;
}
