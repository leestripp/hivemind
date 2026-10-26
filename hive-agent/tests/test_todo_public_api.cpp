/**
 * @file test_todo_public_api.cpp
 * @brief Tests for the public hive::Agent todo API.
 *
 * Hermetic: uses a temporary project directory and a null sink.
 */

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "library/Agent.h"
#include "api/ProviderPlugin.h"
#include "api/ProviderRegistry.h"
#include "library/model_discovery.h"
#include "a2a/A2aClient.h"
#include <filesystem>

using json = nlohmann::json;
namespace fs = std::filesystem;

struct TempProject {
    fs::path dir;

    explicit TempProject(std::string_view suffix) {
        dir = fs::temp_directory_path() /
              ("daedalus_todo_api_test_" + std::string(suffix));
        fs::remove_all(dir);
        fs::create_directories(dir);
    }

    ~TempProject() { fs::remove_all(dir); }
};

class NullSink : public hive::AgentSink {
public:
    void on_assistant_delta(std::string_view) noexcept override {}
    void on_tool_call(std::string_view, std::string_view,
                      const nlohmann::json&) noexcept override {}
    void on_tool_result(std::string_view, std::string_view,
                        const nlohmann::json&) noexcept override {}
    void on_state_change(int, int) noexcept override {}
    void on_log(hive::LogLevel, std::string_view) noexcept override {}
    void on_bell() noexcept override {}
};

static hive::Agent make_agent(const fs::path& project_root,
                                  std::string_view agent_name = {}) {
    hive::AgentConfig cfg;
    cfg.project_root = project_root;
    cfg.agent_name = agent_name;
    cfg.mcp_enabled = false;
    cfg.memory_enabled = false;
    auto sink = std::make_shared<NullSink>();
    return hive::Agent(std::move(cfg), std::move(sink));
}

TEST_SUITE("Agent public todo API") {

    TEST_CASE("add / edit / delete / list / count round-trip") {
        TempProject tp("roundtrip");
        auto agent = make_agent(tp.dir);

        json add = agent.add_todo("Fix build");
        CHECK(add["status"] == "success");
        CHECK(add["todo"]["id"] == 1);
        CHECK(add["todo"]["text"] == "Fix build");

        json edit = agent.edit_todo(1, std::nullopt, "done");
        CHECK(edit["status"] == "success");
        CHECK(edit["todo"]["status"] == "done");

        json list = agent.list_todos("all");
        CHECK(list["status"] == "success");
        CHECK(list["total"] == 1);
        CHECK(list["done"] == 1);

        json count = agent.count_todos();
        CHECK(count["status"] == "success");
        CHECK(count["total"] == 1);
        CHECK(count["max_todos"] == 100);

        json del = agent.delete_todo(1);
        CHECK(del["status"] == "success");
        CHECK(agent.count_todos()["total"] == 0);
    }

    TEST_CASE("set_max_todos and clear_todos") {
        TempProject tp("max_clear");
        auto agent = make_agent(tp.dir);

        CHECK(agent.set_max_todos(2)["status"] == "success");
        CHECK(agent.add_todo("a")["status"] == "success");
        CHECK(agent.add_todo("b")["status"] == "success");
        CHECK(agent.add_todo("c")["status"] == "error");

        json cleared = agent.clear_todos();
        CHECK(cleared["status"] == "success");
        CHECK(cleared["removed"] == 2);
        CHECK(agent.count_todos()["total"] == 0);
    }

    TEST_CASE("Per-agent isolation through public API") {
        TempProject tp("isolation");
        auto agent_a = make_agent(tp.dir, "agent_a");
        auto agent_b = make_agent(tp.dir, "agent_b");

        CHECK(agent_a.add_todo("Task A")["status"] == "success");
        CHECK(agent_b.add_todo("Task B")["status"] == "success");

        json count_a = agent_a.count_todos();
        json count_b = agent_b.count_todos();
        CHECK(count_a["total"] == 1);
        CHECK(count_b["total"] == 1);

        CHECK(fs::exists(tp.dir / ".hive-agent" / "agent_a" / "todos.json"));
        CHECK(fs::exists(tp.dir / ".hive-agent" / "agent_b" / "todos.json"));
    }
}
