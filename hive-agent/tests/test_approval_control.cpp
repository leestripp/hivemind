// SPDX-License-Identifier: BSL-1.0
// tests/test_approval_control.cpp
//
// §BUG-006/007/008 regression tests for the tool-approval fixes:
//
//   BUG-006: AgentConfig::auto_approve is std::optional and AUTHORITATIVE
//            when set. A host that sets it to false must get approval
//            prompts even when the shared SettingsStore row says true.
//            Unset = legacy store fallback.
//   BUG-007: AgentImpl applies the effective value to ToolHandler's global
//            auto-mode flag UNCONDITIONALLY (true and false), so a
//            host-forced OFF is not silently re-enabled by a sticky global
//            or by the LLMClient ctor.
//   BUG-008: hosts get control over / visibility into the approval caches:
//              - Agent::clear_session_approvals()
//              - Agent::clear_persisted_approvals()
//              - Agent::set_remember_approvals()
//              - AgentSink::on_command_auto_approved(command, source)
//
// Hermetic: no LLM, no network. Uses temp project roots + temp
// HIVE_AGENT_HOME per phase, and drives ToolHandler directly for the
// sink-notification checks (the test links the internal headers like
// test_bash_background.cpp does).

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <unistd.h>

#include "library/Agent.h"
#include "api/ProviderPlugin.h"
#include "api/ProviderRegistry.h"
#include "library/model_discovery.h"
#include "a2a/A2aClient.h"
#include "core/settings.h"

#include "tools/ToolHandler.h"

namespace fs = std::filesystem;

#define EXPECT(cond) do {                                              \
    if (!(cond)) {                                                      \
        std::cerr << "FAIL @ " << __LINE__ << ": " #cond << "\n";     \
        ++failures;                                                     \
    }                                                                   \
} while (0)

namespace {

fs::path fresh_dir(const char* tag) {
    fs::path p = fs::temp_directory_path() /
                 (std::string("daedalus-approval-") + tag + "-" +
                  std::to_string(std::chrono::system_clock::now()
                                    .time_since_epoch().count()) +
                  "-" + std::to_string(::getpid()));
    fs::create_directories(p);
    return p;
}

// Sink that records approval prompts and auto-approval notifications.
struct ApprovalSink : public hive::AgentSink {
    int prompts = 0;
    int auto_approved = 0;
    hive::ApprovalSource last_source = hive::ApprovalSource::AutoApprove;
    std::string last_command;
    bool approve = true;

    bool request_command_approval(std::string_view) override {
        ++prompts;
        return approve;
    }
    void on_command_auto_approved(std::string_view command,
                                  hive::ApprovalSource source) noexcept override {
        ++auto_approved;
        last_source = source;
        last_command = command;
    }
};

// RAII restore of the process-global ToolHandler state this test pokes.
struct ToolHandlerStateGuard {
    ~ToolHandlerStateGuard() {
        ToolHandler::set_auto_mode(false);
        ToolHandler::set_remember_approvals(true);
        ToolHandler::set_sink(nullptr);
        ToolHandler::clear_session_approved_commands();
    }
};

int test_config_authoritative_over_store() {
    int failures = 0;

    fs::path home = fresh_dir("bug6-home");
    {
        auto store = hive::SettingsStore::for_home(home);
        store.set_auto_approve(true);   // stale "on" row, e.g. from daedalus-cli --auto
    }

    fs::path project = fresh_dir("bug6-proj");

    hive::AgentConfig cfg;
    cfg.project_root  = project;
    cfg.agent_home = home;
    cfg.mcp_enabled   = false;
    cfg.auto_approve  = false;          // host explicitly wants prompts ON

    auto sink = std::make_shared<ApprovalSink>();
    hive::Agent agent(cfg, sink);
    agent.initialize();

    // BUG-006: host config wins over the store row.
    EXPECT(agent.auto_approve() == false);
    // BUG-007: the ToolHandler global must reflect that (not left ON).
    EXPECT(ToolHandler::is_auto_mode_enabled() == false);

    if (failures == 0) std::cout << "[ok-1] cfg.auto_approve=false overrides store=true\n";
    return failures;
}

int test_legacy_store_fallback() {
    int failures = 0;

    fs::path home = fresh_dir("legacy-home");
    {
        auto store = hive::SettingsStore::for_home(home);
        store.set_auto_approve(true);
    }

    fs::path project = fresh_dir("legacy-proj");

    hive::AgentConfig cfg;
    cfg.project_root  = project;
    cfg.agent_home = home;
    cfg.mcp_enabled   = false;
    // cfg.auto_approve deliberately UNSET → legacy store fallback.

    auto sink = std::make_shared<ApprovalSink>();
    hive::Agent agent(cfg, sink);
    agent.initialize();

    EXPECT(agent.auto_approve() == true);
    EXPECT(ToolHandler::is_auto_mode_enabled() == true);
    ToolHandler::set_auto_mode(false);  // leave clean for later phases

    if (failures == 0) std::cout << "[ok-2] unset cfg.auto_approve falls back to store\n";
    return failures;
}

int test_runtime_toggle() {
    int failures = 0;

    fs::path home = fresh_dir("toggle-home");
    {
        auto store = hive::SettingsStore::for_home(home);
        store.set_auto_approve(false);
    }

    fs::path project = fresh_dir("toggle-proj");

    hive::AgentConfig cfg;
    cfg.project_root  = project;
    cfg.agent_home = home;
    cfg.mcp_enabled   = false;

    auto sink = std::make_shared<ApprovalSink>();
    hive::Agent agent(cfg, sink);
    agent.initialize();

    EXPECT(agent.auto_approve() == false);

    agent.set_auto_approve(true);
    EXPECT(agent.auto_approve() == true);
    EXPECT(ToolHandler::is_auto_mode_enabled() == true);

    agent.set_auto_approve(false);
    EXPECT(agent.auto_approve() == false);
    EXPECT(ToolHandler::is_auto_mode_enabled() == false);

    if (failures == 0) std::cout << "[ok-3] set_auto_approve toggles at runtime\n";
    return failures;
}

int test_auto_approval_notifications() {
    int failures = 0;
    ToolHandlerStateGuard guard;

    auto sink = std::make_shared<ApprovalSink>();
    ToolHandler::set_sink(sink);
    ToolHandler::set_auto_mode(false);
    ToolHandler::clear_session_approved_commands();

    fs::path project = fresh_dir("notify-proj");

    // Session cache hit → approved without prompt, host told SessionCache.
    ToolHandler::add_session_approved_command_internal("git status");
    EXPECT(ToolHandler::request_command_approval("git status") == true);
    EXPECT(sink->prompts == 0);
    EXPECT(sink->auto_approved == 1);
    EXPECT(sink->last_source == hive::ApprovalSource::SessionCache);
    EXPECT(sink->last_command == "git status");

    // Persisted cache hit → host told PersistedCache.
    ToolHandler::add_persisted_approval(project, "", "make all");
    EXPECT(ToolHandler::request_command_approval("make all") == true);
    EXPECT(sink->prompts == 0);
    EXPECT(sink->auto_approved == 2);
    EXPECT(sink->last_source == hive::ApprovalSource::PersistedCache);

    // Auto mode → host told AutoApprove.
    ToolHandler::set_auto_mode(true);
    EXPECT(ToolHandler::request_command_approval("rm -rf /tmp/xyz") == true);
    EXPECT(sink->prompts == 0);
    EXPECT(sink->auto_approved == 3);
    EXPECT(sink->last_source == hive::ApprovalSource::AutoApprove);
    ToolHandler::set_auto_mode(false);

    // Uncached command with auto mode OFF → prompt path, no notification.
    EXPECT(ToolHandler::request_command_approval("cat /etc/hostname") == true);
    EXPECT(sink->prompts == 1);
    EXPECT(sink->auto_approved == 3);

    ToolHandler::clear_persisted_approvals(project, "");

    if (failures == 0) std::cout << "[ok-4] on_command_auto_approved reports correct sources\n";
    return failures;
}

int test_remember_approvals_switch() {
    int failures = 0;
    ToolHandlerStateGuard guard;

    fs::path home = fresh_dir("remember-home");
    fs::path project = fresh_dir("remember-proj");

    hive::AgentConfig cfg;
    cfg.project_root  = project;
    cfg.agent_home = home;
    cfg.mcp_enabled   = false;
    cfg.auto_approve  = false;

    auto sink = std::make_shared<ApprovalSink>();
    sink->approve = true;

    hive::Agent agent(cfg, sink);
    agent.initialize();
    agent.set_remember_approvals(false);

    // The agent has no LLMClient (no creds), so the sink is not wired
    // through ToolHandler yet — wire it directly (that's what
    // LLMClient::set_sink does when a client exists).
    ToolHandler::set_sink(sink);
    ToolHandler::clear_session_approved_commands();

    const std::string cmd = "echo remember-test";

    // First run: prompted, approved, but NOT remembered.
    auto r1 = agent.execute_tool("execute_bash", {{"command", cmd}});
    EXPECT(r1.value("status", "") == "success");
    EXPECT(sink->prompts == 1);
    EXPECT(ToolHandler::is_command_session_approved(cmd) == false);

    // Second run: prompted again ("ask every time" semantics).
    auto r2 = agent.execute_tool("execute_bash", {{"command", cmd}});
    EXPECT(r2.value("status", "") == "success");
    EXPECT(sink->prompts == 2);

    // Re-enable remembering: third run prompts, fourth hits the cache.
    agent.set_remember_approvals(true);
    auto r3 = agent.execute_tool("execute_bash", {{"command", cmd}});
    EXPECT(r3.value("status", "") == "success");
    EXPECT(sink->prompts == 3);
    EXPECT(ToolHandler::is_command_session_approved(cmd) == true);

    auto r4 = agent.execute_tool("execute_bash", {{"command", cmd}});
    EXPECT(r4.value("status", "") == "success");
    EXPECT(sink->prompts == 3);   // no new prompt — cache hit
    EXPECT(sink->auto_approved >= 1);
    EXPECT(sink->last_source == hive::ApprovalSource::SessionCache);

    if (failures == 0) std::cout << "[ok-5] set_remember_approvals(false) = ask every time\n";
    return failures;
}

int test_clear_apis() {
    int failures = 0;
    ToolHandlerStateGuard guard;

    fs::path home = fresh_dir("clear-home");
    fs::path project = fresh_dir("clear-proj");

    hive::AgentConfig cfg;
    cfg.project_root  = project;
    cfg.agent_home = home;
    cfg.mcp_enabled   = false;
    cfg.auto_approve  = false;

    auto sink = std::make_shared<ApprovalSink>();
    hive::Agent agent(cfg, sink);
    agent.initialize();

    // Seed both caches.
    ToolHandler::clear_session_approved_commands();
    ToolHandler::add_session_approved_command_internal("session-cmd");
    ToolHandler::add_persisted_approval(project, "", "persisted-cmd");
    EXPECT(ToolHandler::is_command_session_approved("session-cmd") == true);
    EXPECT(ToolHandler::is_command_session_approved("persisted-cmd") == true);

    agent.clear_session_approvals();
    EXPECT(ToolHandler::is_command_session_approved("session-cmd") == false);
    EXPECT(ToolHandler::is_command_session_approved("persisted-cmd") == true);

    agent.clear_persisted_approvals();
    EXPECT(ToolHandler::is_command_session_approved("persisted-cmd") == false);

    if (failures == 0) std::cout << "[ok-6] clear_session/persisted_approvals work\n";
    return failures;
}

}  // namespace

int main() {
    int failures = 0;
    failures += test_config_authoritative_over_store();
    failures += test_legacy_store_fallback();
    failures += test_runtime_toggle();
    failures += test_auto_approval_notifications();
    failures += test_remember_approvals_switch();
    failures += test_clear_apis();

    if (failures == 0) {
        std::cout << "All approval-control tests passed\n";
        return 0;
    }
    std::cerr << failures << " check(s) FAILED\n";
    return 1;
}
