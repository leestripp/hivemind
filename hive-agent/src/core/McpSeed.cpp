#include "core/McpSeed.h"

#include <db/Database.h>  // OmniNet::DB::Database — sqlite3 wrapper

namespace hive::mcp_seed {

void seed_hub_mcp_servers(
    const std::filesystem::path& home,
    const nlohmann::json& servers,
    const std::function<void(const std::string&)>& log)
{
    try {
        std::error_code ec;
        std::filesystem::create_directories(home, ec);
        const auto db_path = (home / "hive-agent.db").string();

        OmniNet::DB::Database db;
        db.open(db_path);

        // Same shape as SettingsStore's mcp_servers table (v6 schema,
        // with origin). IF NOT EXISTS — the store owns the canonical
        // schema; this just guarantees the table is there when the
        // seed runs before the store's first open.
        db.execute(
            "CREATE TABLE IF NOT EXISTS mcp_servers ("
            " name TEXT PRIMARY KEY, transport TEXT NOT NULL DEFAULT 'stdio',"
            " command TEXT, args_json TEXT, env_json TEXT, url TEXT,"
            " bearer_token TEXT, description TEXT,"
            " enabled INTEGER NOT NULL DEFAULT 1,"
            " origin TEXT NOT NULL DEFAULT 'local')");
        // Older DBs (pre-v6) lack origin — the SettingsStore migration
        // would add it later, but the seed runs first. Idempotent.
        bool upgraded = false;
        try {
            db.execute("ALTER TABLE mcp_servers ADD COLUMN "
                       "origin TEXT NOT NULL DEFAULT 'local'");
            upgraded = true;
        } catch (...) {
            // Column already exists — fine.
        }
        if (upgraded) {
            // Pre-v6 DB: every existing row came from the old hub seed
            // (self-service did not exist yet) — mark them hub so the
            // DELETE below manages them correctly.
            db.execute("UPDATE mcp_servers SET origin = 'hub'");
        }

        // Hub-config-authoritative for HUB rows only: anything the
        // roster no longer declares is dropped; local rows (Phase 71
        // agent self-service) survive.
        db.execute("DELETE FROM mcp_servers WHERE origin = 'hub'");

        int seeded = 0;
        if (servers.is_array()) {
            for (const auto& srv : servers) {
                const std::string name    = srv.value("name", "");
                const std::string command = srv.value("command", "");
                const std::string url     = srv.value("url", "");
                if (name.empty() || (command.empty() && url.empty()))
                    continue;
                const std::string transport =
                    url.empty() ? "stdio" : "streamable-http";
                const std::string args =
                    srv.value("args", nlohmann::json::array()).dump();
                const std::string env =
                    srv.value("env", nlohmann::json::object()).dump();
                auto stmt = db.prepare(
                    "INSERT OR REPLACE INTO mcp_servers (name, transport,"
                    " command, args_json, env_json, url, enabled, origin)"
                    " VALUES (?,?,?,?,?,?,1,'hub')");
                stmt.bind(1, name);
                stmt.bind(2, transport);
                stmt.bind(3, command);
                stmt.bind(4, args);
                stmt.bind(5, env);
                stmt.bind(6, url);
                stmt.step();
                ++seeded;
            }
        }
        log("mcp seed: " + std::to_string(seeded) +
            " hub server(s) seeded (local rows preserved)");
    } catch (const std::exception& e) {
        log(std::string("mcp seed: ") + e.what());
    } catch (...) {
        log("mcp seed: unknown error");
    }
}

}  // namespace hive::mcp_seed
