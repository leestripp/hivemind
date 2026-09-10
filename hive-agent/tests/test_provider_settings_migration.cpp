// SPDX-License-Identifier: BSL-1.0
// tests/test_provider_settings_migration.cpp
//
// §ProviderPlugins Phase 2: providers.plugin column.
//   PM1. Fresh store: plugin round-trips through upsert/get/list.
//   PM2. Plugin defaults to '' (auto-detect) when not set.
//   PM3. resolve_model() passes the provider's plugin hint through.
//   PM4. v4 -> v5 migration: a providers table without the plugin column
//        gains it; existing rows get '' (auto-detect).

#include <chrono>
#include <filesystem>
#include <iostream>
#include <string>
#include <unistd.h>

#include <db/Database.h>   // OmniNet — build a synthetic v4 database

#include "core/settings.h"

namespace fs = std::filesystem;

namespace {

int failures = 0;
#define EXPECT(cond) do { \
    if (!(cond)) { \
        std::cerr << "FAIL " << __FILE__ << ":" << __LINE__ \
                  << " EXPECT(" #cond ")\n"; \
        ++failures; \
    } \
} while (0)

struct TmpHome {
    fs::path path;
    explicit TmpHome(const std::string& tag) {
        path = fs::temp_directory_path() /
               ("daedalus-pm-" + tag + "-" +
                std::to_string(std::chrono::system_clock::now()
                                 .time_since_epoch().count()) + "-" +
                std::to_string(::getpid()));
        fs::remove_all(path);
        fs::create_directories(path);
    }
    ~TmpHome() { std::error_code ec; fs::remove_all(path, ec); }
};

void test_pm1_plugin_roundtrip() {
    TmpHome home("rt");
    auto store = hive::SettingsStore::for_home(home.path);

    hive::ProviderConfig p;
    p.name  = "test-provider";
    p.host  = "https://api.test.example";
    p.api_key = "sk-test";
    p.plugin = "minimax";
    store.upsert_provider(p);

    auto got = store.get_provider("test-provider");
    EXPECT(got.has_value());
    EXPECT(got->plugin == "minimax");

    bool found = false;
    for (const auto& lp : store.list_providers_detailed()) {
        if (lp.name == "test-provider") {
            found = true;
            EXPECT(lp.plugin == "minimax");
        }
    }
    EXPECT(found);

    // Upsert with a different plugin replaces it.
    p.plugin = "moonshot";
    store.upsert_provider(p);
    got = store.get_provider("test-provider");
    EXPECT(got.has_value() && got->plugin == "moonshot");
    std::cout << "[ok] PM1: plugin round-trips through upsert/get/list\n";
}

void test_pm2_default_empty() {
    TmpHome home("def");
    auto store = hive::SettingsStore::for_home(home.path);

    hive::ProviderConfig p;
    p.name  = "plain";
    p.host  = "https://api.plain.example";
    p.api_key = "k";
    // plugin left default ('')
    store.upsert_provider(p);

    auto got = store.get_provider("plain");
    EXPECT(got.has_value());
    EXPECT(got->plugin.empty());
    std::cout << "[ok] PM2: plugin defaults to '' (auto-detect)\n";
}

void test_pm3_resolve_model_passthrough() {
    TmpHome home("res");
    auto store = hive::SettingsStore::for_home(home.path);

    hive::ProviderConfig p;
    p.name = "prov"; p.host = "https://h"; p.api_key = "k"; p.plugin = "lmstudio";
    store.upsert_provider(p);
    hive::ModelConfig m;
    m.name = "mod"; m.provider = "prov"; m.model_id = "m-1";
    store.upsert_model(m);

    auto r = store.resolve_model("mod");
    EXPECT(r.has_value());
    EXPECT(r->plugin == "lmstudio");
    std::cout << "[ok] PM3: resolve_model passes plugin through\n";
}

void test_pm4_v4_to_v5_migration() {    TmpHome home("mig");
    const auto db_path = (home.path / "hive-agent.db").string();

    // Hand-build a v4-shaped database: providers WITHOUT the plugin
    // column, schema_version = 4.
    {
        OmniNet::DB::Database db;
        db.open(db_path);
        db.execute(
            "CREATE TABLE providers ("
            "name TEXT PRIMARY KEY, "
            "host TEXT NOT NULL, "
            "api_key TEXT NOT NULL, "
            "chat_endpoint TEXT NOT NULL DEFAULT '/v1/chat/completions', "
            "discovery_endpoint TEXT NOT NULL DEFAULT '/v1/models', "
            "timeout_seconds INTEGER NOT NULL DEFAULT 30)");
        db.execute("INSERT INTO providers(name, host, api_key) "
                   "VALUES('legacy', 'https://legacy.example', 'sk-old')");
        db.execute("CREATE TABLE schema_version (version INTEGER PRIMARY KEY)");
        db.execute("INSERT INTO schema_version(version) VALUES (4)");
        db.close();
    }

    // Opening the store must run migrate_to_v5.
    auto store = hive::SettingsStore::for_home(home.path);
    auto got = store.get_provider("legacy");
    EXPECT(got.has_value());
    EXPECT(got->host == "https://legacy.example");
    EXPECT(got->plugin.empty());  // '' = auto-detect at runtime

    // And the column must be writable afterwards.
    hive::ProviderConfig p = *got;
    p.plugin = "minimax";
    store.upsert_provider(p);
    got = store.get_provider("legacy");
    EXPECT(got.has_value() && got->plugin == "minimax");
    std::cout << "[ok] PM4: v4 -> v5 migration adds plugin column\n";
}

void test_pm5_endpoint_defaults() {
    TmpHome home("ep");
    auto store = hive::SettingsStore::for_home(home.path);

    // Write path: upserting with blank endpoints stores the plugin's
    // defaults instead of "".
    hive::ProviderConfig p;
    p.name = "blank-ep"; p.host = "https://h"; p.api_key = "k";
    store.upsert_provider(p);
    auto got = store.get_provider("blank-ep");
    EXPECT(got.has_value());
    EXPECT(got->chat_endpoint == "/v1/chat/completions");
    EXPECT(got->discovery_endpoint == "/v1/models");

    // Read path: a legacy row with empty endpoints (pre-fix writes) must
    // still resolve to the plugin's endpoints.
    {
        OmniNet::DB::Database db;
        db.open((home.path / "hive-agent.db").string());
        db.execute("UPDATE providers SET chat_endpoint='', discovery_endpoint='' "
                   "WHERE name='blank-ep'");
        db.close();
    }
    hive::ModelConfig m;
    m.name = "ep-mod"; m.provider = "blank-ep"; m.model_id = "m-1";
    store.upsert_model(m);
    auto r = store.resolve_model("ep-mod");
    EXPECT(r.has_value());
    EXPECT(r->chat_endpoint == "/v1/chat/completions");
    EXPECT(r->discovery_endpoint == "/v1/models");
    std::cout << "[ok] PM5: empty endpoints fall back to plugin defaults\n";
}

} // namespace

int main() {
    test_pm1_plugin_roundtrip();
    test_pm2_default_empty();
    test_pm3_resolve_model_passthrough();
    test_pm4_v4_to_v5_migration();
    test_pm5_endpoint_defaults();

    if (failures) {
        std::cerr << "\ntest_provider_settings_migration: " << failures
                  << " FAILED (of 5 tests)\n";
        return 1;
    }
    std::cout << "\ntest_provider_settings_migration: 5 tests passed\n";
    return 0;
}
