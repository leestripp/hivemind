// SPDX-License-Identifier: BSL-1.0
// tests/test_custom_tool.cpp
//
// §CUSTOM-TOOLS acceptance: hive::BaseTool can be subclassed,
// registered with Agent::register_tool(), and executed both directly
// (Agent::execute_tool) and via the tool registry (list_tools).

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <chrono>
#include <filesystem>
#include <memory>
#include <string>

#include "library/Agent.h"
#include "api/ProviderPlugin.h"
#include "api/ProviderRegistry.h"
#include "library/model_discovery.h"
#include "a2a/A2aClient.h"
#include "core/settings.h"
#include <nlohmann/json.hpp>

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {

struct NullSink : public hive::AgentSink {
    void on_log(hive::LogLevel, std::string_view) noexcept override {}
};

fs::path make_temp_home(const std::string& tag) {
    fs::path p = fs::temp_directory_path() /
        ("hive_agent_test_custom_tool_" + tag + "_" +
         std::to_string(std::chrono::system_clock::now().time_since_epoch().count()));
    fs::remove_all(p);
    fs::create_directories(p);
    return p;
}

void write_settings(const fs::path& agent_home) {
    auto store = hive::SettingsStore::for_home(agent_home);
    hive::ProviderConfig p;
    p.name          = "test-provider";
    p.host          = "http://localhost";
    p.api_key       = "test-key";
    p.chat_endpoint = "/v1/chat/completions";
    store.upsert_provider(p);

    hive::ModelConfig m;
    m.name             = "test-model";
    m.provider         = "test-provider";
    m.model_id         = "test-model-id";
    m.max_context_size = 10000;
    m.temperature      = 0.0;
    store.upsert_model(m);
    store.set_default_model_name("test-model");
}

class MultiplyTool : public hive::BaseTool {
public:
    std::string get_name() const override { return "multiply"; }

    std::string get_description() const override {
        return "Multiply two numbers and return the product.";
    }

    json get_definition() const override {
        return {
            {"type", "function"},
            {"function", {
                {"name", "multiply"},
                {"description", get_description()},
                {"parameters", {
                    {"type", "object"},
                    {"properties", {
                        {"a", {{"type", "number"}, {"description", "First factor"}}},
                        {"b", {{"type", "number"}, {"description", "Second factor"}}}
                    }},
                    {"required", {"a", "b"}}
                }}
            }}
        };
    }

    json execute(const json& args, hive::HostToolContext& /*ctx*/) override {
        double a = args.value("a", 0.0);
        double b = args.value("b", 0.0);
        return {
            {"status", "success"},
            {"product", a * b}
        };
    }
};

}  // namespace

TEST_CASE("custom BaseTool is registered and executable") {
    fs::path home = make_temp_home("base");
    write_settings(home);

    fs::path project = home / "project";
    fs::create_directories(project);

    hive::AgentConfig cfg;
    cfg.project_root  = project;
    cfg.agent_home = home;
    cfg.session_id    = "custom-tool";

    auto sink = std::make_shared<NullSink>();
    hive::Agent agent(std::move(cfg), sink);

    // Register before initialize — the library queues and registers on init.
    agent.register_tool(std::make_unique<MultiplyTool>());
    agent.initialize();

    // The custom tool appears in the tool catalog.
    auto tools = agent.list_tools();
    bool found = false;
    for (const auto& t : tools) {
        if (t.name == "multiply") {
            found = true;
            break;
        }
    }
    CHECK(found);

    // Direct execution works without LLM involvement.
    json result = agent.execute_tool("multiply", json{{"a", 6}, {"b", 7}});
    CHECK(result.value("status", "") == "success");
    CHECK(result.value("product", 0.0) == doctest::Approx(42.0));

    agent.shutdown();
    fs::remove_all(home);
}

TEST_CASE("custom BaseTool can be registered after initialize") {
    fs::path home = make_temp_home("after");
    write_settings(home);

    fs::path project = home / "project";
    fs::create_directories(project);

    hive::AgentConfig cfg;
    cfg.project_root  = project;
    cfg.agent_home = home;
    cfg.session_id    = "custom-tool-after";

    auto sink = std::make_shared<NullSink>();
    hive::Agent agent(std::move(cfg), sink);
    agent.initialize();

    // Register after init — should still become available.
    agent.register_tool(std::make_unique<MultiplyTool>());

    json result = agent.execute_tool("multiply", json{{"a", 3}, {"b", 8}});
    CHECK(result.value("status", "") == "success");
    CHECK(result.value("product", 0.0) == doctest::Approx(24.0));

    agent.shutdown();
    fs::remove_all(home);
}
