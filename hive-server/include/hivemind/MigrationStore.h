#pragma once

// =============================================================================
// MigrationStore (Phase 73): durable agent-migration jobs. See Plan.md
// (Agent Migration) and docs/design.md.
//
// A migration moves an agent's runtime between workers (container or
// bare-metal, including cross-type). The hub orchestrates; this store is
// the durable job record that survives restarts — the same pattern as
// DeliveryStore (Phase 66): one row per job, a live-state machine, a
// sweeper that re-drives in-flight jobs whose worker op died mid-flight.
//
// State machine:
//   pending -> exporting -> transferring -> provisioning -> importing ->
//   verifying -> cleanup -> done
// plus terminal: failed (error + retryable), cancelled, rolled_back.
// One LIVE job per agent at a time (create() enforces).
// =============================================================================

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace hivemind {

/// One migration job.
struct Migration {
    std::string id;
    std::string agent_id;
    std::string agent_name;      // denormalized for history display
    std::string source_worker;
    std::string target_worker;
    std::string source_type;     // "container" | "host" (pre-migration runtime)
    std::string target_type;     // "container" | "host"
    std::string state;           // see banner
    std::string error;           // last error ("" when none)
    bool        retryable = false;
    bool        force = false;       // may stop a busy agent
    bool        no_rollback = false; // stay put on target failure
    std::string export_path;     // worker-side tar path (metadata only)
    long long   export_size = 0;
    std::string progress;        // short step note for clients
    std::string created_at;
    std::string updated_at;
};

class MigrationStore {
public:
    MigrationStore();                                   // default XDG path
    explicit MigrationStore(std::filesystem::path db);  // tests / custom
    ~MigrationStore();

    // ── jobs ──────────────────────────────────────────────────────────
    /// Insert a new job (state=pending when empty). Generates the id when
    /// empty and stamps created_at/updated_at. Throws std::invalid_argument
    /// when a LIVE job already exists for this agent.
    std::string create(Migration m);
    std::optional<Migration> get(const std::string& id);
    /// Newest first. Empty agent_id = all.
    std::vector<Migration> list(const std::string& agent_id = "", int limit = 50);
    /// State transition; stamps updated_at. Clears error when empty.
    void set_state(const std::string& id, const std::string& state,
                   const std::string& error = "", const std::string& progress = "");
    void set_export(const std::string& id, const std::string& path, long long size);
    /// The single live job for an agent, if any (concurrency gate).
    std::optional<Migration> live_job_for(const std::string& agent_id);
    /// All in-flight jobs (pending..cleanup) — hub sweeper input.
    std::vector<Migration> inflight();

    /// True while the job holds its agent slot / needs re-driving.
    static bool is_live(const std::string& state);

private:
    void init_schema();
    std::filesystem::path db_path_;
};

}  // namespace hivemind
