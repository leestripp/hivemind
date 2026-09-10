// SPDX-License-Identifier: BSL-1.0
// tests/test_settings_active_model.cpp
//
// Regression test: verifies that Agent::active_model() works when using
// SettingsStore::instance() (the default). Previously the webui demo set
// cfg.agent_home to a legacy path, causing AgentImpl to use
// SettingsStore::for_home() instead, which opened an empty DB.
//
// This test ensures:
//   1. SettingsStore::instance() can list models
//   2. Agent created WITHOUT agent_home set can find models via active_model()
//   3. Agent with no model_name falls back to the store's default model
//
// Adapted for hive-agent (post-merge): HERMETIC. The daedalus version read
// the real user DB; post-merge the model registry is hub-owned and the local
// store may legitimately be empty, so we seed our own via HIVE_AGENT_HOME
// (must be set BEFORE the first SettingsStore::instance() call — the
// singleton resolves its DB path lazily on first use).

#include <cstdlib>
#include <iostream>
#include <string>
#include <filesystem>

#include "library/Agent.h"
#include "api/ProviderPlugin.h"
#include "api/ProviderRegistry.h"
#include "library/model_discovery.h"
#include "a2a/A2aClient.h"
#include "core/settings.h"

namespace fs = std::filesystem;

int main() {
    int failures = 0;

    // Hermetic store: redirect the singleton to a scratch home BEFORE any use.
    const fs::path tmp = fs::temp_directory_path() / "hive_settings_active_model_test";
    fs::remove_all(tmp);
    fs::create_directories(tmp);
    setenv("HIVE_AGENT_HOME", tmp.c_str(), 1);

    // Seed: one provider, one model, marked default.
    {
        auto& store = hive::SettingsStore::instance();
        hive::ProviderConfig p;
        p.name          = "test-provider";
        p.host          = "http://test.invalid/test-provider";
        p.api_key       = "sk-test-test-provider";
        p.chat_endpoint = "/v1/chat/completions";
        if (!store.get_provider(p.name).has_value()) {
            store.upsert_provider(p);
        }
        hive::ModelConfig m;
        m.name             = "test-seeded-model";
        m.provider         = "test-provider";
        m.model_id         = "model-id-test-seeded-model";
        m.max_context_size = 123456;
        m.temperature      = 0.42;
        store.upsert_model(m);
        store.set_default_model_name("test-seeded-model");
    }

    // S1: SettingsStore::instance() lists the seeded model
    {
        const auto& store = hive::SettingsStore::instance();
        auto models = store.list_models();
        std::cout << "[S1] SettingsStore::instance() has " << models.size() << " models\n";
        if (models.empty()) {
            std::cerr << "[S1] FAIL: seeded model missing from instance()\n";
            ++failures;
        }
    }

    // S2: Agent::active_model() works WITHOUT agent_home set
    {
        hive::AgentConfig cfg;
        cfg.project_root = "/tmp";
        cfg.model_name = "test-seeded-model";

        struct NullSink : hive::AgentSink {
            void on_log(hive::LogLevel, std::string_view) noexcept override {}
            void on_assistant_delta(std::string_view) noexcept override {}
            void on_tool_call(std::string_view, const nlohmann::json&) noexcept override {}
            void on_tool_result(std::string_view, const nlohmann::json&) noexcept override {}
            void on_state_change(int, int) noexcept override {}
        };

        auto sink = std::make_shared<NullSink>();
        hive::Agent agent(cfg, sink);

        try {
            agent.initialize();
        } catch (const std::exception& e) {
            std::cerr << "[S2] Agent::initialize() threw: " << e.what() << "\n";
            ++failures;
        }

        auto m = agent.active_model();
        std::cout << "[S2] Agent::active_model() returned:\n";
        std::cout << "  name: '" << m.name << "'\n";
        std::cout << "  model_id: '" << m.model_id << "'\n";
        std::cout << "  host: '" << m.host << "'\n";
        std::cout << "  max_context_size: " << m.max_context_size << "\n";

        if (m.name.empty()) {
            std::cerr << "[S2] FAIL: active_model() returned empty!\n";
            ++failures;
        }
    }

    // S3: Agent::active_model() falls back to default model when model_name is empty
    {
        hive::AgentConfig cfg;
        cfg.project_root = "/tmp";
        // No model_name set - should use default from settings

        struct NullSink : hive::AgentSink {
            void on_log(hive::LogLevel, std::string_view) noexcept override {}
            void on_assistant_delta(std::string_view) noexcept override {}
            void on_tool_call(std::string_view, const nlohmann::json&) noexcept override {}
            void on_tool_result(std::string_view, const nlohmann::json&) noexcept override {}
            void on_state_change(int, int) noexcept override {}
        };

        auto sink = std::make_shared<NullSink>();
        hive::Agent agent(cfg, sink);

        try {
            agent.initialize();
        } catch (const std::exception& e) {
            std::cerr << "[S3] Agent::initialize() threw: " << e.what() << "\n";
            ++failures;
        }

        auto m = agent.active_model();
        std::cout << "[S3] Agent with no model_name, active_model() returned:\n";
        std::cout << "  name: '" << m.name << "'\n";
        std::cout << "  model_id: '" << m.model_id << "'\n";

        const auto& store = hive::SettingsStore::instance();
        auto models = store.list_models();
        if (!models.empty() && m.name.empty()) {
            std::cerr << "[S3] FAIL: store has " << models.size() << " models but active_model() returned empty!\n";
            ++failures;
        } else if (!m.name.empty()) {
            std::cout << "[S3] OK: active_model() returned a model (default from store)\n";
        } else {
            std::cout << "[S3] OK: no default model set (acceptable for fresh DB)\n";
        }
    }

    fs::remove_all(tmp);

    if (failures) {
        std::cerr << "test_settings_active_model: " << failures << " FAILED\n";
        return 1;
    }
    std::cout << "test_settings_active_model: all checks PASSED\n";
    return 0;
}
