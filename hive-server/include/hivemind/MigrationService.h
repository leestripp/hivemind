#pragma once

// =============================================================================
// MigrationService (Phase 73): hub-side orchestrator for agent migration
// between workers. See Plan.md (Agent Migration) — design doc.
//
// The hub stays dumb: this service only drives worker-channel frames and
// flips the roster `worker`/`host_agent` fields at the verification gate.
// All state moves as ONE tar.zst per job, relayed source → hub → target in
// chunks (the hub never persists blob bytes — metadata only, same posture
// as Phase 66 file sharing).
//
// Worker protocol (frames on the worker channel):
//   hub → source   {"op":"agent_export","job_id","agent_id","name","force"}
//   source → hub   {"op":"agent_export_ready","job_id","path","size"}
//                  {"op":"agent_export_error","job_id","error"}
//   hub → source   {"op":"agent_data_begin","job_id","target_worker"}
//   source → hub   {"op":"agent_data_chunk","job_id","seq","data_b64","last"}
//   hub → target   (chunk forwarded verbatim)
//   target → hub   {"op":"agent_data_done","job_id","path"}
//                  {"op":"agent_data_error","job_id","error"}
//   hub → target   {"op":"agent_import","job_id","agent_id","target_type",
//                   "name",<spawn-spec fields>}
//   target → hub   {"op":"agent_import_done","job_id"}
//                  {"op":"agent_import_error","job_id","error"}
//   hub → source   {"op":"agent_cleanup","job_id","agent_id","name"}
//   source → hub   {"op":"agent_cleanup_done","job_id"}
//   hub → either   {"op":"agent_migrate_abort","job_id"}  (cancel; staging only)
//
// Every op is idempotent per job_id (worker staging dir keyed by job), so
// the sweep thread can safely re-drive a state after a hub restart.
// =============================================================================

#include <atomic>
#include <map>
#include <mutex>
#include <string>
#include <thread>

#include <nlohmann/json.hpp>

#include "hivemind/MigrationStore.h"

// Peer services live in the GLOBAL namespace (same forward-declaration
// convention as HubRouter.h) — not in hivemind.
class AgentStore;
class ChatService;
class HubRouter;
class WorkerChannel;

namespace hivemind {

class MigrationService {
public:
    MigrationService(MigrationStore& store, AgentStore& agents,
                     WorkerChannel& channel, HubRouter& hub);
    ~MigrationService();  // stops the sweep thread

    MigrationService(const MigrationService&) = delete;
    MigrationService& operator=(const MigrationService&) = delete;

    void set_chat_service(ChatService* cs) { chat_ = cs; }

    /// Validate + create the job + kick the async driver.
    /// Throws std::invalid_argument with a user-facing message on refusal.
    std::string begin(const std::string& agent_id,
                      const std::string& target_worker,
                      const std::string& target_type, bool force,
                      bool no_rollback, const std::string& ack);

    /// Cancel a live job. Before the roster flip this just abandons
    /// staging (agent untouched); after the flip it performs a rollback.
    /// Throws std::invalid_argument when the job is not live.
    void cancel(const std::string& job_id);

    /// Worker reply pump — main.cpp calls this alongside hub.on_worker_frame.
    void on_worker_frame(const std::string& worker, const nlohmann::json& f);

    /// Boot sweep: re-drive in-flight jobs (restart-resume).
    void sweep();

    MigrationStore& store() { return store_; }

private:
    void sweep_loop();
    void drive(const Migration& job);          // act on the current state
    void fail(const Migration& job, const std::string& error);
    void rollback(const Migration& job, const std::string& why);
    void flip_roster(const Migration& job);    // worker + host_agent fields
    void send_import(const Migration& job);    // agent_import + spawn spec
    void send(const std::string& worker, const nlohmann::json& f);

    MigrationStore& store_;
    AgentStore& agents_;
    WorkerChannel& channel_;
    HubRouter& hub_;
    ChatService* chat_ = nullptr;

    std::atomic<bool> stop_{false};
    std::thread sweeper_;

    // Verifying bookkeeping: job_id -> deadline (steady-clock epoch secs).
    std::mutex m_;
    std::map<std::string, std::int64_t> verifying_deadline_;
    static constexpr std::int64_t kVerifyTimeoutS = 180;
    static constexpr std::int64_t kSweepIntervalS = 2;
};

}  // namespace hivemind
