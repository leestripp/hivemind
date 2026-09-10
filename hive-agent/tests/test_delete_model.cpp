// SPDX-License-Identifier: BSL-1.0
// tests/test_delete_model.cpp
//
// Agent::delete_model() — public-API passthrough to
// SettingsStore::delete_model(). Covers the happy path, the empty-name
// guard, and idempotence for unknown names.

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

namespace fs = std::filesystem;

namespace {

struct NullSink : public hive::AgentSink {
    void on_log(hive::LogLevel, std::string_view) noexcept override {}
};

fs::path make_temp_home(const std::string& tag) {
    fs::path p = fs::temp_directory_path() /
        ("hive_agent_test_delete_model_" + tag + "_" +
         std::to_string(std::chrono::system_clock::now().time_since_epoch().count()));
    fs::remove_all(p);
    fs::create_directories(p);
    return p;
}

void write_settings(const fs::path& agent_home) {
    auto store = hive::SettingsStore::for_home(agent_home);
    hive::ProviderConfig p;
    p.name          = "p1";
    p.host          = "http://p1.example";
    p.api_key       = "p1-key";
    p.chat_endpoint = "/v1/chat/completions";
    store.upsert_provider(p);

    hive::ModelConfig m;
    m.name             = "small";
    m.provider         = "p1";
    m.model_id         = "small-model";
    m.max_context_size = 10000;
    m.temperature      = 0.0;
    store.upsert_model(m);
    store.set_default_model_name("small");
}

bool model_exists(const fs::path& home, const std::string& name) {
    auto store = hive::SettingsStore::for_home(home);
    return store.get_model(name).has_value();
}

}  // namespace

TEST_CASE("delete_model removes a configured model") {
    fs::path home = make_temp_home("happy");
    write_settings(home);

    fs::path project = home / "project";
    fs::create_directories(project);

    hive::AgentConfig cfg;
    cfg.project_root  = project;
    cfg.agent_home = home;
    cfg.session_id    = "delete-happy";

    auto sink = std::make_shared<NullSink>();
    hive::Agent agent(std::move(cfg), sink);
    agent.initialize();

    REQUIRE(model_exists(home, "small"));

    CHECK(agent.delete_model("small"));
    CHECK_FALSE(model_exists(home, "small"));

    agent.shutdown();
    fs::remove_all(home);
}

TEST_CASE("delete_model rejects an empty name") {
    fs::path home = make_temp_home("empty");
    write_settings(home);

    fs::path project = home / "project";
    fs::create_directories(project);

    hive::AgentConfig cfg;
    cfg.project_root  = project;
    cfg.agent_home = home;
    cfg.session_id    = "delete-empty";

    auto sink = std::make_shared<NullSink>();
    hive::Agent agent(std::move(cfg), sink);
    agent.initialize();

    CHECK_FALSE(agent.delete_model(""));
    CHECK(model_exists(home, "small"));

    agent.shutdown();
    fs::remove_all(home);
}

TEST_CASE("delete_model on an unknown name is idempotent") {
    fs::path home = make_temp_home("unknown");
    write_settings(home);

    fs::path project = home / "project";
    fs::create_directories(project);

    hive::AgentConfig cfg;
    cfg.project_root  = project;
    cfg.agent_home = home;
    cfg.session_id    = "delete-unknown";

    auto sink = std::make_shared<NullSink>();
    hive::Agent agent(std::move(cfg), sink);
    agent.initialize();

    // The store's DELETE is a no-op for missing rows; the API reports
    // success (same semantics as delete_provider).
    CHECK(agent.delete_model("nonexistent"));
    CHECK(model_exists(home, "small"));

    agent.shutdown();
    fs::remove_all(home);
}
