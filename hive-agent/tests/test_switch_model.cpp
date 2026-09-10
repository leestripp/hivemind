// SPDX-License-Identifier: BSL-1.0
// tests/test_switch_model.cpp
//
// §MODEL-SWITCH acceptance: Agent::switch_model() changes the active
// model, updates the SettingsStore default, reconfigures the internal
// LLMClient, and clears the conversation history so the new model
// starts with a fresh context.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
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
        ("hive_agent_test_switch_model_" + tag + "_" +
         std::to_string(std::chrono::system_clock::now().time_since_epoch().count()));
    fs::remove_all(p);
    fs::create_directories(p);
    return p;
}

hive::ProviderConfig make_provider(const std::string& name,
                                        const std::string& host) {
    hive::ProviderConfig p;
    p.name          = name;
    p.host          = host;
    p.api_key       = name + "-key";
    p.chat_endpoint = "/v1/chat/completions";
    return p;
}

hive::ModelConfig make_model(const std::string& name,
                                 [[maybe_unused]] const std::string& host,
                                 int max_context_size,
                                 const std::string& provider = "") {
    hive::ModelConfig m;
    m.name             = name;
    m.provider         = provider;
    m.model_id         = name + "-model";
    m.max_context_size = max_context_size;
    m.temperature      = 0.0;
    return m;
}

void write_settings(const fs::path& agent_home) {
    auto store = hive::SettingsStore::for_home(agent_home);
    store.upsert_provider(make_provider("p1", "http://p1.example"));
    store.upsert_provider(make_provider("p2", "http://p2.example"));
    store.upsert_model(make_model("small", "http://small.example", 10000, "p1"));
    store.upsert_model(make_model("large", "http://large.example", 100000, "p1"));
    store.set_default_model_name("small");
}

}  // namespace

TEST_CASE("switch_model rejects unknown model and leaves state intact") {
    fs::path home = make_temp_home("reject");
    write_settings(home);

    fs::path project = home / "project";
    fs::create_directories(project);

    hive::AgentConfig cfg;
    cfg.project_root  = project;
    cfg.agent_home = home;
    cfg.session_id    = "switch-reject";

    auto sink = std::make_shared<NullSink>();
    hive::Agent agent(std::move(cfg), sink);
    agent.initialize();

    CHECK(agent.active_model().name == "small");

    // Unknown model must fail.
    bool switched = agent.switch_model("nonexistent");
    CHECK_FALSE(switched);

    // Active model and default must be unchanged.
    CHECK(agent.active_model().name == "small");
    auto store = hive::SettingsStore::for_home(home);
    CHECK(store.get_default_model_name() == "small");

    agent.shutdown();
    fs::remove_all(home);
}

TEST_CASE("switch_model clears conversation history") {
    fs::path home = make_temp_home("clear");
    write_settings(home);

    fs::path project = home / "project";
    fs::create_directories(project);

    // Pre-seed a messages.json in the new layout path.
    fs::path agent_dir = project / ".hive-agent" / "default";
    fs::create_directories(agent_dir);
    fs::path messages_file = agent_dir / "messages.json";
    {
        json history = json::array();
        history.push_back({{"role", "system"}, {"content", "you are a test assistant"}});
        history.push_back({{"role", "user"}, {"content", "hello"}});
        history.push_back({{"role", "assistant"}, {"content", "hi"}});
        std::ofstream f(messages_file);
        f << history.dump(2);
    }
    REQUIRE(fs::is_regular_file(messages_file));

    hive::AgentConfig cfg;
    cfg.project_root  = project;
    cfg.agent_home = home;
    cfg.session_id    = "switch-clear";

    auto sink = std::make_shared<NullSink>();
    hive::Agent agent(std::move(cfg), sink);
    agent.initialize();

    // The pre-seeded history was loaded.
    CHECK(agent.stats().message_count == 3);
    CHECK(agent.last_assistant_text() == "hi");

    // Switching to a valid model clears history.
    bool switched = agent.switch_model("large");
    CHECK(switched);
    CHECK(agent.active_model().name == "large");
    CHECK(agent.stats().message_count == 0);
    CHECK(agent.last_assistant_text().empty());

    // The on-disk transcript was removed.
    CHECK_FALSE(fs::exists(messages_file));

    // Default model in the store is updated.
    auto store = hive::SettingsStore::for_home(home);
    CHECK(store.get_default_model_name() == "large");

    agent.shutdown();
    fs::remove_all(home);
}

TEST_CASE("switch_model updates compression config for new context size") {
    fs::path home = make_temp_home("ctx");
    write_settings(home);

    fs::path project = home / "project";
    fs::create_directories(project);

    hive::AgentConfig cfg;
    cfg.project_root  = project;
    cfg.agent_home = home;
    cfg.session_id    = "switch-ctx";

    auto sink = std::make_shared<NullSink>();
    hive::Agent agent(std::move(cfg), sink);
    agent.initialize();

    CHECK(agent.active_model().max_context_size == 10000);

    bool switched = agent.switch_model("large");
    CHECK(switched);
    CHECK(agent.active_model().max_context_size == 100000);

    agent.shutdown();
    fs::remove_all(home);
}


TEST_CASE("switch_provider persists provider and switches to a model of that provider") {
    fs::path home = make_temp_home("provider");
    {
        auto store = hive::SettingsStore::for_home(home);
        store.upsert_provider(make_provider("p1", "http://p1.example"));
        store.upsert_provider(make_provider("p2", "http://p2.example"));
        store.upsert_model(make_model("p1-a", "http://p1.example", 10000, "p1"));
        store.upsert_model(make_model("p1-b", "http://p1.example", 20000, "p1"));
        store.upsert_model(make_model("p2-a", "http://p2.example", 30000, "p2"));
        store.set_default_model_name("p1-a");
    }

    fs::path project = home / "project";
    fs::create_directories(project);

    hive::AgentConfig cfg;
    cfg.project_root  = project;
    cfg.agent_home = home;
    cfg.session_id    = "switch-provider";

    auto sink = std::make_shared<NullSink>();
    hive::Agent agent(std::move(cfg), sink);
    agent.initialize();

    CHECK(agent.active_model().name == "p1-a");
    CHECK(agent.list_providers() == std::vector<std::string>{"p1", "p2"});

    bool switched = agent.switch_provider("p2");
    CHECK(switched);
    CHECK(agent.active_model().name == "p2-a");
    CHECK(hive::SettingsStore::for_home(home).get_current_provider() == "p2");

    // Unknown provider fails.
    CHECK_FALSE(agent.switch_provider("p3"));

    agent.shutdown();
    fs::remove_all(home);
}

TEST_CASE("switch_model creates the LLMClient when none existed at startup") {
    fs::path home = make_temp_home("create-client");
    {
        // Providers and models exist, but NO default model is configured —
        // initialize() must skip LLMClient creation.
        auto store = hive::SettingsStore::for_home(home);
        store.upsert_provider(make_provider("p1", "http://p1.example"));
        store.upsert_model(make_model("small", "http://small.example", 10000, "p1"));
    }

    fs::path project = home / "project";
    fs::create_directories(project);

    hive::AgentConfig cfg;
    cfg.project_root  = project;
    cfg.agent_home = home;
    cfg.session_id    = "switch-create-client";

    auto sink = std::make_shared<NullSink>();
    hive::Agent agent(std::move(cfg), sink);
    agent.initialize();

    // No client at startup: stats report a zero context size.
    CHECK(agent.stats().max_context_size == 0);

    // Switching to a valid configured model must create the client,
    // otherwise run_one_turn() fails with "no model configured" forever.
    bool switched = agent.switch_model("small");
    CHECK(switched);
    CHECK(agent.active_model().name == "small");
    CHECK(agent.stats().max_context_size == 10000);

    agent.shutdown();
    fs::remove_all(home);
}

TEST_CASE("switch_model creates config for discovered provider model id") {
    fs::path home = make_temp_home("discovered");
    {
        auto store = hive::SettingsStore::for_home(home);
        store.upsert_provider(make_provider("kimi", "http://kimi.example"));
        store.upsert_model(make_model("kimi-ref", "http://kimi.example", 256000, "kimi"));
        store.set_current_provider("kimi");
        store.set_default_model_name("kimi-ref");
    }

    fs::path project = home / "project";
    fs::create_directories(project);

    hive::AgentConfig cfg;
    cfg.project_root  = project;
    cfg.agent_home = home;
    cfg.session_id    = "switch-discovered";

    auto sink = std::make_shared<NullSink>();
    hive::Agent agent(std::move(cfg), sink);
    agent.initialize();

    CHECK(agent.active_model().name == "kimi-ref");

    // Switch to a model id that is not yet configured; the library should
    // synthesize a ModelConfig from the current provider's host/api_key.
    bool switched = agent.switch_model("kimi-k2.5");
    CHECK(switched);
    CHECK(agent.active_model().name == "kimi-k2.5");
    CHECK(agent.active_model().model_id == "kimi-k2.5");

    auto store = hive::SettingsStore::for_home(home);
    auto saved = store.get_model("kimi-k2.5");
    REQUIRE(saved.has_value());
    CHECK(saved->provider == "kimi");

    auto resolved = store.resolve_model("kimi-k2.5");
    REQUIRE(resolved.has_value());
    CHECK(resolved->host == "http://kimi.example");

    agent.shutdown();
    fs::remove_all(home);
}
