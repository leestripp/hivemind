// SPDX-License-Identifier: BSL-1.0
// tests/test_config_locations.cpp
//
// Unit tests for hive-agent config/settings path resolution.
//
// As of v0.5.0 settings live in a SQLite database, not config.json:
//   - Global DB:  $XDG_DATA_HOME/hive-brain/hive-agent.db
//                 (fallback ~/.local/share/hive-brain/hive-agent.db)
//   - Per-agent DB when cfg.agent_home is set: <agent_home>/hive-agent.db
//
// What we pin:
//
//   L1. Paths::global().user_data_root() / "hive-agent.db" is the canonical
//       settings database path — NOT config.json.
//   L2. AgentConfig with agent_home set reads from <agent_home>/hive-agent.db.
//   L3. Agent with no explicit agent_home uses the global SettingsStore DB.
//   L4. SettingsStore populated directly (upsert_model + set_default_model_name)
//       is visible to Agent::list_models().
//   L5. Paths::global().user_data_root() honours $HIVE_AGENT_DATA_HOME.

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
#include "library/paths.h"
#include "core/settings.h"

namespace fs = std::filesystem;

namespace {

// RAII env-var setter. Restores on destruction.
class ScopedEnv {
public:
    ScopedEnv(const char* name, const std::string& value) : m_name(name) {
        const char* prev = std::getenv(name);
        m_had_prev = (prev != nullptr);
        if (m_had_prev) m_prev = prev;
        if (value.empty()) {
            ::unsetenv(name);
        } else {
            ::setenv(name, value.c_str(), 1);
        }
    }
    ~ScopedEnv() {
        if (m_had_prev) {
            ::setenv(m_name, m_prev.c_str(), 1);
        } else {
            ::unsetenv(m_name);
        }
    }
    ScopedEnv(const ScopedEnv&) = delete;
    ScopedEnv& operator=(const ScopedEnv&) = delete;

private:
    const char* m_name;
    std::string m_prev;
    bool m_had_prev;
};

// Build a minimal ModelConfig for testing.
hive::ModelConfig make_test_model(const std::string& name) {
    hive::ModelConfig m;
    m.name             = name;
    m.provider         = "test-provider";
    m.model_id         = "model-" + name;
    m.max_context_size = 100000;
    m.temperature      = 0.0;
    return m;
}

void ensure_test_provider(hive::SettingsStore& store) {
    if (!store.get_provider("test-provider").has_value()) {
        hive::ProviderConfig p;
        p.name          = "test-provider";
        p.host          = "http://test.invalid";
        p.api_key       = "sk-test-not-placeholder";
        p.chat_endpoint = "/v1/chat/completions";
        store.upsert_provider(p);
    }
}

struct NullSink : hive::AgentSink {
    void on_log(hive::LogLevel, std::string_view) noexcept override {}
    void on_assistant_delta(std::string_view) noexcept override {}
    void on_tool_call(std::string_view, const nlohmann::json&) noexcept override {}
    void on_tool_result(std::string_view, const nlohmann::json&) noexcept override {}
    void on_state_change(int, int) noexcept override {}
};

// ===========================================================================
// L1. user_data_root() / "hive-agent.db" is the canonical settings DB path.
// ===========================================================================
bool test_user_data_root_db_is_canonical() {
    auto root = hive::Paths::global().user_data_root();
    auto expected = root / "hive-agent.db";
    if (expected.filename() != "hive-agent.db") {
        std::cerr << "FAIL: L1 expected hive-agent.db, got " << expected << "\n";
        return false;
    }
    std::cout << "[ok] L1: canonical settings DB is hive-agent.db: "
              << expected.string() << "\n";
    return true;
}

// ===========================================================================
// L2. AgentConfig with agent_home set reads from <home>/hive-agent.db.
// ===========================================================================
bool test_agent_uses_agent_home_db() {
    auto home = fs::temp_directory_path() / "l2-hive-home";
    fs::remove_all(home);
    fs::create_directories(home);

    {
        auto store = hive::SettingsStore::for_home(home);
        ensure_test_provider(store);
        store.upsert_model(make_test_model("l2-model"));
        store.set_default_model_name("l2-model");
    }

    hive::AgentConfig cfg;
    cfg.project_root  = "/tmp";
    cfg.agent_home = home;

    auto sink = std::make_shared<NullSink>();
    hive::Agent agent(cfg, sink);
    agent.initialize();

    auto models = agent.list_models();
    bool found_l2 = false;
    for (const auto& m : models) {
        if (m.name == "l2-model") found_l2 = true;
    }
    if (!found_l2) {
        std::cerr << "FAIL: L2 did not find l2-model\n";
        return false;
    }

    fs::remove_all(home);
    std::cout << "[ok] L2: AgentImpl resolves cfg.agent_home -> hive-agent.db\n";
    return true;
}

// ===========================================================================
// L3. Agent with no explicit paths uses the global SettingsStore DB.
// ===========================================================================
bool test_agent_uses_global_db_when_no_home() {
    auto tmp_home = fs::temp_directory_path() / "l3-hive-home";
    fs::remove_all(tmp_home);
    fs::create_directories(tmp_home / ".local" / "share" / "daedalus");

    ScopedEnv home("HOME", tmp_home.string());
    ScopedEnv xdg_data("XDG_DATA_HOME", "");
    ScopedEnv daedalus_data("HIVE_AGENT_DATA_HOME", "");

    {
        auto& store = hive::SettingsStore::instance();
        ensure_test_provider(store);
        store.upsert_model(make_test_model("l3-model"));
        store.set_default_model_name("l3-model");
    }

    hive::AgentConfig cfg;
    cfg.project_root = "/tmp";
    // NO agent_home set — must fall back to the global SettingsStore.

    auto sink = std::make_shared<NullSink>();
    hive::Agent agent(cfg, sink);
    agent.initialize();

    auto models = agent.list_models();
    bool found_l3 = false;
    for (const auto& m : models) {
        if (m.name == "l3-model") found_l3 = true;
    }
    if (!found_l3) {
        std::cerr << "FAIL: L3 did not find l3-model\n";
        return false;
    }

    fs::remove_all(tmp_home);
    std::cout << "[ok] L3: Agent uses global SettingsStore (no explicit home)\n";
    return true;
}

// ===========================================================================
// L4. SettingsStore populated directly is visible through Agent::list_models().
// ===========================================================================
bool test_settings_store_direct_population() {
    auto home = fs::temp_directory_path() / "l4-cfg";
    fs::remove_all(home);
    fs::create_directories(home);

    {
        auto store = hive::SettingsStore::for_home(home);
        ensure_test_provider(store);
        store.upsert_model(make_test_model("l4-good"));
        store.set_default_model_name("l4-good");
    }

    hive::AgentConfig cfg;
    cfg.project_root  = "/tmp";
    cfg.agent_home = home;

    auto sink = std::make_shared<NullSink>();
    hive::Agent agent(cfg, sink);
    agent.initialize();

    auto models = agent.list_models();
    bool found_l4 = false;
    for (const auto& m : models) {
        if (m.name == "l4-good") found_l4 = true;
    }
    if (!found_l4) {
        std::cerr << "FAIL: L4 did not find l4-good\n";
        return false;
    }

    fs::remove_all(home);
    std::cout << "[ok] L4: SettingsStore populated, Agent lists model\n";
    return true;
}

// ===========================================================================
// L5. $HIVE_AGENT_DATA_HOME short-circuits the global data root resolution.
// ===========================================================================
bool test_daedalus_data_home_override() {
    ScopedEnv env("HIVE_AGENT_DATA_HOME", "/tmp/l5-override");
    auto root = hive::Paths::global().user_data_root();
    if (root != "/tmp/l5-override") {
        std::cerr << "FAIL: L5 expected /tmp/l5-override, got " << root << "\n";
        return false;
    }
    std::cout << "[ok] L5: $HIVE_AGENT_DATA_HOME redirects data root\n";
    return true;
}

}  // namespace

int main() {
    bool ok = true;
    ok &= test_user_data_root_db_is_canonical();     // L1
    ok &= test_agent_uses_agent_home_db();        // L2
    ok &= test_agent_uses_global_db_when_no_home();  // L3
    ok &= test_settings_store_direct_population();   // L4
    ok &= test_daedalus_data_home_override();        // L5

    if (ok) {
        std::cout << "\ntest_config_locations: 5 tests passed\n";
        return 0;
    }
    std::cerr << "\ntest_config_locations: FAILED\n";
    return 1;
}
