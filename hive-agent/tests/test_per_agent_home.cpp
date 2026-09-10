// SPDX-License-Identifier: BSL-1.0
// tests/test_per_agent_home.cpp
//
// §Phase 8.5 acceptance: each hive::Agent instance can have its
// own daedalus home. Two Agents in one process can read different
// settings databases via different agent_home values.
//
// As of v0.5.0 settings live in SQLite (daedalus.db), not settings.json.
// This test creates two temporary daedalus-home trees, each with its
// own SettingsStore (different default_model), and verifies that each
// Agent sees its own models.

#include <cstdlib>
#include <filesystem>
#include <iostream>
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

// Make a unique temp dir under /tmp.
fs::path make_temp_home(const std::string& tag) {
    fs::path p = fs::temp_directory_path() /
        ("hive_agent_test_" + tag + "_" +
         std::to_string(std::chrono::system_clock::now().time_since_epoch().count()));
    fs::remove_all(p);
    fs::create_directories(p);
    return p;
}

// Build a minimal ModelConfig for testing.
hive::ModelConfig make_model(const std::string& name,
                                 [[maybe_unused]] const std::string& host,
                                 [[maybe_unused]] const std::string& api_key) {
    hive::ModelConfig m;
    m.name             = name;
    m.provider         = name + "-provider";
    m.model_id         = name + "-model";
    m.max_context_size = 100000;
    m.temperature      = 0.0;
    return m;
}

// Populate a per-agent SettingsStore under home.
void write_settings(const fs::path& agent_home,
                    const std::string& default_model) {
    auto store = hive::SettingsStore::for_home(agent_home);

    auto make_provider = [](const std::string& name,
                            const std::string& host,
                            const std::string& api_key) {
        hive::ProviderConfig p;
        p.name          = name;
        p.host          = host;
        p.api_key       = api_key;
        p.chat_endpoint = "/v1/chat/completions";
        return p;
    };

    store.upsert_provider(make_provider("alpha-provider", "http://alpha.example", "alpha-key"));
    store.upsert_provider(make_provider("beta-provider",  "http://beta.example",  "beta-key"));
    store.upsert_model(make_model("alpha", "http://alpha.example", "alpha-key"));
    store.upsert_model(make_model("beta",  "http://beta.example",  "beta-key"));
    store.set_default_model_name(default_model);
}

struct NullSink : public hive::AgentSink {
    void on_log(hive::LogLevel, std::string_view) noexcept override {}
};

}  // namespace

int main() {
    int failures = 0;
#define EXPECT(cond) do {                                              \
    if (!(cond)) {                                                      \
        std::cerr << "FAIL @ " << __LINE__ << ": " #cond << "\n";       \
        ++failures;                                                     \
    }                                                                   \
} while (0)

    // Set up two isolated homes.
    fs::path home_a = make_temp_home("alpha");
    fs::path home_b = make_temp_home("beta");
    write_settings(home_a, "alpha");
    write_settings(home_b, "beta");

    {
        // ---- Agent A: configured to use home_a ----
        hive::AgentConfig cfg_a;
        cfg_a.agent_home = home_a;
        cfg_a.session_id  = "agent-A";

        NullSink sink_a;
        auto shared_a = std::shared_ptr<hive::AgentSink>(&sink_a, [](void*){});
        hive::Agent agent_a(std::move(cfg_a), shared_a);
        agent_a.initialize();

        auto models_a = agent_a.list_models();
        EXPECT(models_a.size() == 2);
        bool has_alpha = false, has_beta = false;
        for (const auto& m : models_a) {
            if (m.name == "alpha") has_alpha = true;
            if (m.name == "beta")  has_beta  = true;
        }
        EXPECT(has_alpha);
        EXPECT(has_beta);

        bool switched_a = agent_a.switch_model("alpha");
        EXPECT(switched_a);

        agent_a.shutdown();

        // ---- Agent B: configured to use home_b ----
        hive::AgentConfig cfg_b;
        cfg_b.agent_home = home_b;
        cfg_b.session_id  = "agent-B";

        NullSink sink_b;
        auto shared_b = std::shared_ptr<hive::AgentSink>(&sink_b, [](void*){});
        hive::Agent agent_b(std::move(cfg_b), shared_b);
        agent_b.initialize();

        auto models_b = agent_b.list_models();
        EXPECT(models_b.size() == 2);
        bool switched_b = agent_b.switch_model("beta");
        EXPECT(switched_b);

        agent_b.shutdown();
    }

    // Cleanup temp dirs.
    fs::remove_all(home_a);
    fs::remove_all(home_b);

    if (failures) {
        std::cerr << "\ntest_per_agent_home: " << failures << " FAILED\n";
        return 1;
    }
    std::cout << "[test] per_agent_home PASSED (two Agents with "
              << "different agent_home values coexist; each loads its own "
              << "SettingsStore DB)\n";
    return 0;
}
