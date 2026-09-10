// End-to-end: write a model into the SQLite SettingsStore, then read it
// through the Agent (which reads from SettingsStore directly).
// This validates the wire-in is real.
#include <cassert>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <string>

#include "library/Agent.h"
#include "api/ProviderPlugin.h"
#include "api/ProviderRegistry.h"
#include "library/model_discovery.h"
#include "a2a/A2aClient.h"
#include "core/settings.h"

namespace fs = std::filesystem;

int main() {
    // Stage 1: put a model into the SQLite store via the PUBLIC API.
    {
        // §B2 fix (v0.5.1): instance() returns by reference (true singleton).
        // Writes here propagate to the singleton directly.
        auto& store = hive::SettingsStore::instance();
        hive::ProviderConfig p;
        p.name          = "e2e-provider";
        p.host          = "http://test.invalid";
        p.api_key       = "sk-store-e2e-not-placeholder";
        p.chat_endpoint = "/v1/chat/completions";
        store.upsert_provider(p);

        hive::ModelConfig m;
        m.name             = "store-e2e";
        m.provider         = "e2e-provider";
        m.model_id         = "model-store-e2e";
        m.max_context_size = 50000;
        m.temperature      = 0.5;
        store.upsert_model(m);
        store.set_string("default_model", "store-e2e");
        std::cout << "[stage 1] wrote store-e2e to "
                  << store.db_path().string() << "\n";
    }

    // Stage 2: read it back through Agent (which uses SettingsStore directly).
    {
        hive::AgentConfig cfg;
        cfg.project_root = "/tmp";
        // No agent_home / settings_path — AgentImpl uses the default,
        // which calls SettingsStore::instance() first; the store has our
        // model, so the JSON layer is bypassed.

        struct NullSink : hive::AgentSink {
            void on_log(hive::LogLevel, std::string_view) noexcept override {}
            void on_assistant_delta(std::string_view) noexcept override {}
            void on_tool_call(std::string_view, const nlohmann::json&) noexcept override {}
            void on_tool_result(std::string_view, const nlohmann::json&) noexcept override {}
            void on_state_change(int, int) noexcept override {}
        };
        auto sink = std::make_shared<NullSink>();
        hive::Agent agent(std::move(cfg), sink);
        try {
            agent.initialize();
        } catch (const std::exception& e) {
            std::cerr << "init threw: " << e.what() << "\n";
            return 1;
        }

        auto models = agent.list_models();
        bool found = false;
        for (const auto& m : models) {
            if (m.name == "store-e2e") {
                found = true;
                std::cout << "[stage 2] Agent sees model: " << m.name
                          << " (model_id=" << m.model_id
                          << ", host=" << m.host
                          << ", temperature=" << m.temperature << ")\n";
            }
        }
        if (!found) {
            std::cerr << "FAIL: Agent did not see model written via SettingsStore\n";
            return 1;
        }
    }

    // Cleanup
    hive::SettingsStore::instance().delete_model("store-e2e");
    std::cout << "[ok] end-to-end SettingsStore → Agent list_models\n";
    return 0;
}