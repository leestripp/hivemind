#pragma once

// =============================================================================
// migration_ops — Phase 73 (Plan.md "Agent Migration"): worker-side handlers
// for hub-driven agent moves. Protocol contract lives in the banner of
// hive-server/include/hivemind/MigrationService.h — read that first.
//
// Roles: agent_export / agent_data_begin / agent_cleanup run on the SOURCE
// worker; agent_data_chunk / agent_import run on the TARGET. The hub relays
// chunks verbatim and never persists blob bytes. Staging lives ONLY in the
// worker-owned shared workspace (<ws>/migrations/<job_id>/): host-agent
// homes are hivex-owned, so staging is never placed inside them.
//
// Every op is idempotent per job_id (archives are reused, chunk seq 0
// truncates), so the hub's sweep thread may re-drive any state after a
// restart. Boot prunes the whole staging dir.
// =============================================================================

#include <filesystem>
#include <functional>
#include <string>

#include <nlohmann/json.hpp>

#include "fleet.h"

namespace hwmigration {

namespace fs = std::filesystem;
using json = nlohmann::json;
using SendFn = std::function<void(const json&)>;

/// Fleet interactions injected by main.cpp (keeps this module testable
/// without podman). A null/empty std::function means "not available".
struct Hooks {
    /// Brain process up (any state but down/absent)?
    std::function<bool(const std::string&)> is_running;
    /// Graceful stop of the brain.
    std::function<void(const std::string&)> stop_agent;
    /// Spawn (container or host — spec.host_agent decides).
    std::function<void(const hwfleet::AgentSpec&)> spawn_agent;
    /// Roster-delete teardown (stops brain, destroys container + state).
    std::function<void(const std::string&)> remove_agent;
    /// hivex-<name> user if this agent runs host-side here, else "".
    std::function<std::string(const std::string&)> host_user_of;
    /// Real workspace root for HOST agents ("" = container/unknown —
    /// caller falls back to the shared staging workspace).
    std::function<std::string(const std::string&)> workspace_root_of;
};

struct Ctx {
    fs::path ws;           // shared staging workspace root
    fs::path staging_root; // ws/"migrations"
    SendFn send;
    Hooks hooks;
};

/// Routes one migration frame. Never throws: failures are reported as the
/// protocol's *_error frames (cleanup failures send nothing — the hub's
/// verifying deadline/rollback handles them).
void run_op(const json& frame, const Ctx& ctx);

// ─── pure helpers (exposed for tests) ───────────────────────────────────────
std::string b64_encode(const std::string& in);
std::string b64_decode(const std::string& in);  // throws std::runtime_error

}  // namespace hwmigration
