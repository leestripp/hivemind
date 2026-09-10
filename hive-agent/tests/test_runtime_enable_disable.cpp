// ============================================================================
// tests/test_runtime_enable_disable.cpp
//
// §Phase 16: runtime enable/disable for skills, MCP servers, and tools.
// Verifies that the public Agent API can toggle each category on/off after
// initialize() without recreating the Agent.
//
// All tests are hermetic — no network, no live LLM calls.
// ============================================================================

#include "library/Agent.h"
#include "library/AgentSink.h"
#include "core/settings.h"

#include "core/SkillManager.h"
#include "mcp/McpServerManager.h"
#include "tools/ToolFactory.h"
#include "tools/ToolHandler.h"

#include <algorithm>
#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

struct RecordingSink : hive::AgentSink {
    void on_log(hive::LogLevel, std::string_view) noexcept override {}
    void on_assistant_delta(std::string_view) noexcept override {}
    void on_tool_call(std::string_view, const nlohmann::json&) noexcept override {}
    void on_tool_result(std::string_view, const nlohmann::json&) noexcept override {}
    void on_state_change(int, int) noexcept override {}
};

template <typename T>
bool contains(const std::vector<T>& v, const T& value) {
    return std::find(v.begin(), v.end(), value) != v.end();
}

std::vector<std::string> tool_names(const std::vector<hive::ToolInfo>& tools) {
    std::vector<std::string> names;
    names.reserve(tools.size());
    for (const auto& t : tools) names.push_back(t.name);
    return names;
}

std::vector<std::string> skill_names(const std::vector<hive::SkillInfo>& skills) {
    std::vector<std::string> names;
    names.reserve(skills.size());
    for (const auto& s : skills) names.push_back(s.name);
    return names;
}

// ============================================================================
// Skills
// ============================================================================
void test_runtime_skill_toggle() {
    fs::path tmp = fs::temp_directory_path() / "runtime-skill-toggle";
    fs::remove_all(tmp);
    fs::create_directories(tmp);

    auto write_skill = [&](const std::string& name, const std::string& desc) {
        fs::path d = tmp / name;
        fs::create_directories(d);
        std::ofstream f(d / "SKILL.md");
        f << "---\nname: " << name << "\ndescription: " << desc << "\n---\n\nbody\n";
    };
    write_skill("alpha", "alpha skill");
    write_skill("bravo", "bravo skill");

    hive::AgentConfig cfg;
    cfg.project_root = tmp;
    cfg.skills_dirs  = { tmp };

    auto sink = std::make_shared<RecordingSink>();
    hive::Agent agent(std::move(cfg), sink);
    agent.initialize();

    auto initial_skills = skill_names(agent.list_skills());
    assert(contains(initial_skills, std::string("alpha")));
    assert(contains(initial_skills, std::string("bravo")));

    // Disable alpha at runtime.
    assert(agent.set_skill_enabled("alpha", false));
    assert(!agent.is_skill_enabled("alpha"));
    auto disabled = agent.disabled_skills();
    assert(contains(disabled, std::string("alpha")));

    auto after_disable = skill_names(agent.list_skills());
    assert(!contains(after_disable, std::string("alpha")));
    assert(contains(after_disable, std::string("bravo")));

    // Re-enable alpha.
    assert(agent.set_skill_enabled("alpha", true));
    assert(agent.is_skill_enabled("alpha"));
    auto after_enable = skill_names(agent.list_skills());
    assert(contains(after_enable, std::string("alpha")));
    assert(contains(after_enable, std::string("bravo")));

    // Idempotent disable returns false.
    assert(agent.set_skill_enabled("alpha", false));
    assert(!agent.set_skill_enabled("alpha", false));

    // Unknown skill names are rejected.
    assert(!agent.set_skill_enabled("nonexistent", false));
    assert(!agent.set_skill_enabled("nonexistent", true));

    fs::remove_all(tmp);
    std::cout << "[ok] test_runtime_skill_toggle" << std::endl;
}

// ============================================================================
// MCP servers
// ============================================================================
void test_runtime_mcp_server_toggle() {
    fs::path tmp = fs::temp_directory_path() / "runtime-mcp-toggle";
    fs::remove_all(tmp);
    fs::create_directories(tmp);

    {
        auto store = hive::SettingsStore::for_home(tmp);

        hive::SettingsStore::McpServer a;
        a.name      = "server-a";
        a.transport = "stdio";
        a.command   = "echo";
        a.args      = {"hi"};
        a.enabled   = true;
        store.upsert_mcp_server(a);

        hive::SettingsStore::McpServer b;
        b.name      = "server-b";
        b.transport = "stdio";
        b.command   = "echo";
        b.args      = {"bye"};
        b.enabled   = true;
        store.upsert_mcp_server(b);
    }

    hive::AgentConfig cfg;
    cfg.project_root  = tmp;
    cfg.agent_home = tmp;
    cfg.mcp_enabled   = true;

    auto sink = std::make_shared<RecordingSink>();
    hive::Agent agent(std::move(cfg), sink);
    agent.initialize();

    auto initial_servers = agent.mcp_configured_servers();
    assert(contains(initial_servers, std::string("server-a")));
    assert(contains(initial_servers, std::string("server-b")));

    // Disable server-a at runtime.
    assert(agent.set_mcp_server_enabled("server-a", false));
    assert(!agent.is_mcp_server_enabled("server-a"));
    auto disabled = agent.disabled_mcp_servers();
    assert(contains(disabled, std::string("server-a")));

    auto after_disable = agent.mcp_configured_servers();
    // Adapted for hive-agent: "configured" = present in the store. The
    // enabled/disabled state is tracked separately (disabled_mcp_servers()
    // asserted above; list_connected_servers() is the filtered view).
    assert(contains(after_disable, std::string("server-a")));
    assert(contains(after_disable, std::string("server-b")));

    // Re-enable server-a.
    agent.set_mcp_server_enabled("server-a", true);
    assert(agent.is_mcp_server_enabled("server-a"));
    auto after_enable = agent.mcp_configured_servers();
    assert(contains(after_enable, std::string("server-a")));
    assert(contains(after_enable, std::string("server-b")));

    // Unknown server names are rejected.
    assert(!agent.set_mcp_server_enabled("nonexistent", false));
    assert(!agent.set_mcp_server_enabled("nonexistent", true));

    fs::remove_all(tmp);
    std::cout << "[ok] test_runtime_mcp_server_toggle" << std::endl;
}

// ============================================================================
// Tools
// ============================================================================
void test_runtime_tool_toggle() {
    fs::path tmp = fs::temp_directory_path() / "runtime-tool-toggle";
    fs::remove_all(tmp);
    fs::create_directories(tmp);

    hive::AgentConfig cfg;
    cfg.project_root = tmp;

    auto sink = std::make_shared<RecordingSink>();
    hive::Agent agent(std::move(cfg), sink);
    agent.initialize();

    auto initial_tools = tool_names(agent.list_tools());
    assert(contains(initial_tools, std::string("execute_bash")));

    // Disable execute_bash at runtime.
    assert(agent.set_tool_enabled("execute_bash", false));
    assert(!agent.is_tool_enabled("execute_bash"));
    auto disabled = agent.disabled_tools();
    assert(contains(disabled, std::string("execute_bash")));

    auto after_disable = tool_names(agent.list_tools());
    assert(!contains(after_disable, std::string("execute_bash")));

    // Direct execution of a disabled tool is rejected.
    auto result = agent.execute_tool("execute_bash",
        nlohmann::json::object({{"command", "echo hi"}}));
    assert(result.value("status", "unknown") == "error");
    assert(result.value("message", "").find("disabled") != std::string::npos);

    // Re-enable execute_bash.
    assert(agent.set_tool_enabled("execute_bash", true));
    assert(agent.is_tool_enabled("execute_bash"));
    auto after_enable = tool_names(agent.list_tools());
    assert(contains(after_enable, std::string("execute_bash")));

    // Idempotent enable returns false.
    assert(!agent.set_tool_enabled("execute_bash", true));

    // Unknown tool names are rejected.
    assert(!agent.set_tool_enabled("nonexistent", false));
    assert(!agent.set_tool_enabled("nonexistent", true));

    fs::remove_all(tmp);
    std::cout << "[ok] test_runtime_tool_toggle" << std::endl;
}

// ============================================================================
// Initial disabled sets from AgentConfig still work.
// ============================================================================
void test_initial_disabled_sets() {
    fs::path tmp = fs::temp_directory_path() / "runtime-initial-disabled";
    fs::remove_all(tmp);
    fs::create_directories(tmp);

    auto write_skill = [&](const std::string& name) {
        fs::path d = tmp / name;
        fs::create_directories(d);
        std::ofstream f(d / "SKILL.md");
        f << "---\nname: " << name << "\ndescription: " << name << " desc\n---\n\nbody\n";
    };
    write_skill("blocked-skill");
    write_skill("allowed-skill");

    hive::AgentConfig cfg;
    cfg.project_root   = tmp;
    cfg.skills_dirs    = { tmp };
    cfg.skills_disabled = { "blocked-skill" };
    cfg.tools_disabled  = { "execute_bash" };

    auto sink = std::make_shared<RecordingSink>();
    hive::Agent agent(std::move(cfg), sink);
    agent.initialize();

    auto skills = skill_names(agent.list_skills());
    assert(!contains(skills, std::string("blocked-skill")));
    assert(contains(skills, std::string("allowed-skill")));

    auto tools = tool_names(agent.list_tools());
    assert(!contains(tools, std::string("execute_bash")));

    fs::remove_all(tmp);
    std::cout << "[ok] test_initial_disabled_sets" << std::endl;
}

}  // namespace

int main() {
    test_runtime_skill_toggle();
    test_runtime_mcp_server_toggle();
    test_runtime_tool_toggle();
    test_initial_disabled_sets();
    std::cout << "test_runtime_enable_disable: 4 tests passed\n";
    return 0;
}
