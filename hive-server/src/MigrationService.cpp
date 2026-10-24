// =============================================================================
// MigrationService — see include/hivemind/MigrationService.h (Phase 73,
// Plan.md). Hub-side orchestrator: drives worker-channel frames, flips the
// roster at the verification gate, rolls back on failure. The hub never
// holds blob bytes — chunks are relayed source → hub → target.
// =============================================================================

#include "hivemind/MigrationService.h"

#include <algorithm>
#include <chrono>
#include <iostream>
#include <vector>
#include <stdexcept>

#include "hivemind/AgentStore.h"
#include "hivemind/ChatService.h"
#include "hivemind/HubRouter.h"
#include "hivemind/WorkerChannel.h"

namespace hivemind {
namespace {

using json = nlohmann::json;

std::int64_t now_secs() {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

// Defined just before drive() below; cancel/rollback use it earlier.
std::string agent_name_of(AgentStore& agents, const std::string& id);

}  // namespace

MigrationService::MigrationService(MigrationStore& store, AgentStore& agents,
                                   WorkerChannel& channel, HubRouter& hub)
    : store_(store), agents_(agents), channel_(channel), hub_(hub) {
    sweeper_ = std::thread([this] { sweep_loop(); });
}

MigrationService::~MigrationService() {
    stop_ = true;
    if (sweeper_.joinable()) sweeper_.join();
}

void MigrationService::send(const std::string& worker, const json& f) {
    // M1 live-test observability: the protocol is otherwise silent and
    // a dropped frame looks exactly like a slow worker.
    const bool ok = channel_.send(worker, f);
    std::cerr << "[migration] -> " << worker << " op=" << f.value("op", "")
              << " job=" << f.value("job_id", "")
              << (ok ? "" : " SEND-FAILED") << "\n";
}

// ─── begin: validate + create + kick ─────────────────────────────────────────

std::string MigrationService::begin(const std::string& agent_id,
                                    const std::string& target_worker,
                                    const std::string& target_type, bool force,
                                    bool no_rollback, const std::string& ack) {
    auto a = agents_.get_agent(agent_id);
    if (!a) throw std::invalid_argument("no such agent");

    // Hub-side podman ("local") has no worker channel — out of scope v1.
    if (a->worker == "local" || target_worker == "local")
        throw std::invalid_argument(
            "hub-side (local) agents are not migratable yet");

    const bool src_host = a->host_agent;
    const bool tgt_host = target_type == "host";
    if (target_type != "container" && target_type != "host")
        throw std::invalid_argument("target_type must be container|host");
    if (a->worker == target_worker && src_host == tgt_host)
        throw std::invalid_argument("agent already runs that runtime there");

    if (auto live = store_.live_job_for(agent_id))
        throw std::invalid_argument("migration already in progress (" +
                                    live->id + ")");

    if (!channel_.is_online(a->worker))
        throw std::invalid_argument("source worker '" + a->worker +
                                    "' is offline");
    if (!channel_.is_online(target_worker))
        throw std::invalid_argument("target worker '" + target_worker +
                                    "' is offline");

    if (tgt_host) {
        if (ack != "I UNDERSTAND")
            throw std::invalid_argument(
                "bare-metal target requires typed ack: I UNDERSTAND");
        // Phase 72 capability gate is enforced again at the target
        // worker: the agent_import op refuses when the worker was not
        // started with host-agent support (agent_import_error).
    }

    if (chat_ && chat_->agent_busy(agent_id) && !force)
        throw std::invalid_argument(
            "agent is mid-turn; retry when idle or pass force");

    // Warn-by-default when the target worker does not advertise the
    // agent's model (worker-owned models.json — Plan.md §6.2).
    std::string progress;
    if (!a->model.empty()) {
        bool served = false;
        for (const auto& e : channel_.models_catalog()) {
            if (e.value("name", "") == a->model) { served = true; break; }
        }
        if (!served) progress = "warning: no worker advertises model '" +
                                 a->model + "'; agent may fail to call LLMs";
    }

    Migration m;
    m.agent_id = a->id;
    m.agent_name = a->name;
    m.source_worker = a->worker;
    m.source_type = src_host ? "host" : "container";
    m.target_worker = target_worker;
    m.target_type = target_type;
    m.force = force;
    m.no_rollback = no_rollback;
    m.progress = progress;
    const std::string id = store_.create(m);
    drive(*store_.get(id));
    return id;
}

// ─── cancel ──────────────────────────────────────────────────────────────────

void MigrationService::cancel(const std::string& job_id) {
    auto job = store_.get(job_id);
    if (!job || !MigrationStore::is_live(job->state))
        throw std::invalid_argument("no live migration " + job_id);
    if (job->state == "verifying" || job->state == "cleanup") {
        // Roster already flipped — cancel IS a rollback.
        rollback(*job, "cancelled after placement");
        return;
    }
    if (job->state == "importing") {
        // The extract/spawn may have landed — remove the placement so
        // no orphan copy survives a cancel.
        send(job->target_worker, {{"op", "agent_migrate_teardown"},
                                  {"job_id", job_id},
                                  {"agent_id", job->agent_id},
                                  {"name", agent_name_of(agents_, job->agent_id)}});
    }
    send(job->source_worker, {{"op", "agent_migrate_abort"},
                              {"job_id", job_id}});
    send(job->target_worker, {{"op", "agent_migrate_abort"},
                              {"job_id", job_id}});
    store_.set_state(job_id, "cancelled", "", "cancelled");
}

// ─── worker reply pump ───────────────────────────────────────────────────────

void MigrationService::on_worker_frame(const std::string& worker,
                                       const json& f) {
    const std::string op = f.value("op", "");
    if (op.rfind("agent_", 0) != 0) return;  // not migration traffic
    static const std::vector<std::string> kOps = {
        "agent_export_ready",  "agent_export_error", "agent_data_chunk",
        "agent_data_done",     "agent_data_error",   "agent_import_done",
        "agent_import_error",  "agent_cleanup_done", "agent_cleanup_error",
    };
    if (std::find(kOps.begin(), kOps.end(), op) == kOps.end()) return;

    const std::string job_id = f.value("job_id", "");
    std::cerr << "[migration] <- " << worker << " op=" << op
              << " job=" << job_id << "\n";
    auto job = store_.get(job_id);
    if (!job || !MigrationStore::is_live(job->state)) return;

    if (op == "agent_export_ready") {
        if (worker != job->source_worker) return;  // wrong sender
        store_.set_export(job_id, f.value("path", ""),
                          f.value("size", std::int64_t{0}));
        store_.set_state(job_id, "transferring", "", "streaming state");
        // Source pushes chunks to the hub; the hub relays to the target.
        // begun_ makes the data_begin one-shot per transfer: the 2s
        // sweep would otherwise re-drive it mid-stream and the source
        // would restart at seq 0 while the target appends — a corrupted
        // archive. On hub restart begun_ is empty: the re-driven begin
        // restarts the stream at seq 0 and the target TRUNCATES on that
        // seq, so the transfer converges.
        {
            std::lock_guard<std::mutex> lk(m_);
            begun_.insert(job_id);
        }
        send(job->source_worker, {{"op", "agent_data_begin"},
                                  {"job_id", job_id},
                                  {"target_worker", job->target_worker}});
    } else if (op == "agent_export_error") {
        if (worker != job->source_worker) return;
        {
            std::lock_guard<std::mutex> lk(m_);
            begun_.erase(job_id);
        }
        fail(*job, f.value("error", "export failed"));
    } else if (op == "agent_data_chunk") {
        // Relay verbatim; the hub never parses or stores blob bytes.
        if (worker != job->source_worker) return;
        send(job->target_worker, f);
    } else if (op == "agent_data_done") {
        if (worker != job->target_worker) return;
        {
            std::lock_guard<std::mutex> lk(m_);
            begun_.erase(job_id);
        }
        store_.set_state(job_id, "importing", "", "restoring state");
        send_import(*job);
    } else if (op == "agent_data_error") {
        if (worker != job->target_worker) return;
        {
            std::lock_guard<std::mutex> lk(m_);
            begun_.erase(job_id);
        }
        fail(*job, f.value("error", "transfer failed"));
    } else if (op == "agent_import_done") {
        if (worker != job->target_worker) return;
        // flip_roster/spawn may throw (agent vanished mid-flight): a
        // throw here would unwind into the WS handler thread — catch
        // and fail the job instead.
        try {
            flip_roster(*job);
        } catch (const std::exception& e) {
            return fail(*job, std::string("roster flip: ") + e.what());
        }
        store_.set_state(job_id, "verifying", "", "agent starting on target");
        {
            std::lock_guard<std::mutex> lk(m_);
            verifying_deadline_[job_id] = now_secs() + kVerifyTimeoutS;
        }
        try {
            if (auto moved = agents_.get_agent(job->agent_id))
                hub_.spawn_agent(*moved);
        } catch (const std::exception& e) {
            return fail(*job, std::string("target spawn: ") + e.what());
        }
    } else if (op == "agent_import_error") {
        if (worker != job->target_worker) return;
        fail(*job, f.value("error", "import failed"));
    } else if (op == "agent_cleanup_done") {
        if (worker != job->source_worker) return;
        store_.set_state(job_id, "done", "", "migration complete");
    } else if (op == "agent_cleanup_error") {
        // Agent IS migrated (roster flipped); only the source archive
        // failed — record it, don't roll back a healthy placement.
        store_.set_state(job_id, "failed",
                         "cleanup: " + f.value("error", "unknown"), "");
    }
    (void)worker;
}

void MigrationService::send_import(const Migration& job) {
    // The import op needs enough spawn spec to provision the runtime
    // without a second round-trip; the roster remains the registry of
    // record — these fields mirror spawn_frame (HubRouter).
    auto a = agents_.get_agent(job.agent_id);
    if (!a) { fail(job, "agent vanished mid-migration"); return; }
    send(job.target_worker,
         {{"op", "agent_import"},
          {"job_id", job.id},
          {"agent_id", a->id},
          {"name", a->name},
          {"target_type", job.target_type},
          {"model", a->model},
          {"memory", a->memory},
          {"cpus", a->cpus},
          {"type", a->type}});
}

// ─── roster flip / rollback ──────────────────────────────────────────────────

void MigrationService::flip_roster(const Migration& job) {
    auto a = agents_.get_agent(job.agent_id);
    if (!a) throw std::runtime_error("agent vanished mid-migration");
    const bool tgt_host = job.target_type == "host";
    // Phase 72 policy: bare-metal placement forces the approval gate on.
    agents_.update_agent(a->id, a->name, a->role, a->model, a->workspace_mode,
                         tgt_host ? true : a->require_approval, a->avatar,
                         job.target_worker,   // worker = "" keeps; we set it
                         a->mcp_servers, a->type, a->memory, a->cpus,
                         a->thinking_level, tgt_host ? 1 : 0);
}

void MigrationService::rollback(const Migration& job, const std::string& why) {
    auto a = agents_.get_agent(job.agent_id);
    if (a) {
        const bool src_host = job.source_type == "host";
        try {
            agents_.update_agent(a->id, a->name, a->role, a->model,
                                 a->workspace_mode, a->require_approval,
                                 a->avatar, job.source_worker, a->mcp_servers,
                                 a->type, a->memory, a->cpus, a->thinking_level,
                                 src_host ? 1 : 0);
            if (auto respawned = agents_.get_agent(a->id))
                hub_.spawn_agent(*respawned);
        } catch (const std::exception& e) {
            store_.set_state(job.id, "failed",
                             "rollback incomplete: " + std::string(e.what()),
                             why);
            return;
        }
    }
    // Tear down any placement on the target BEFORE respawning the
    // source — otherwise a half-imported (or fully running) target copy
    // coexists with the respawned source: two live brains, one identity.
    send(job.target_worker, {{"op", "agent_migrate_teardown"},
                             {"job_id", job.id},
                             {"agent_id", job.agent_id},
                             {"name", agent_name_of(agents_, job.agent_id)}});
    send(job.source_worker, {{"op", "agent_migrate_abort"},
                             {"job_id", job.id}});
    send(job.target_worker, {{"op", "agent_migrate_abort"},
                             {"job_id", job.id}});
    store_.set_state(job.id, "rolled_back", "", why);
}

// ─── failure / driver / sweep ────────────────────────────────────────────────

void MigrationService::fail(const Migration& job, const std::string& error) {
    {
        std::lock_guard<std::mutex> lk(m_);
        begun_.erase(job.id);
    }
    store_.set_state(job.id, "failed", error, "");
}

namespace {
// Worker state dirs are keyed by NAME (.hive-agent/<Name>) — the
// export/cleanup frames must carry it.
std::string agent_name_of(AgentStore& agents, const std::string& id) {
    const auto a = agents.get_agent(id);
    return a ? a->name : "";
}
}  // namespace

void MigrationService::drive(const Migration& job) {
    // (Re-)send the frame belonging to the current state. Every op is
    // idempotent per job_id (worker staging keyed by job), so re-driving
    // after a restart is safe.
    const std::string& s = job.state;
    if (s == "pending") {
        store_.set_state(job.id, "exporting", "", "stopping + exporting state");
        send(job.source_worker, {{"op", "agent_export"},
                                 {"job_id", job.id},
                                 {"agent_id", job.agent_id},
                                 {"name", agent_name_of(agents_, job.agent_id)},
                                 {"force", job.force}});
    } else if (s == "transferring") {
        // NEVER re-drive data_begin here: the source is mid-stream and
        // a restart at seq 0 would corrupt the target's append. A live
        // transfer (begun_ armed in this process) simply continues; a
        // stale "transferring" (begun_ empty after a hub restart) is
        // stepped back to "exporting" by the sweep — the worker reuses
        // its archive, re-sends export_ready, and the stream restarts
        // with a seq-0 truncate on the target.
        return;
    } else if (s == "importing") {
        send_import(job);
    } else if (s == "cleanup") {
        send(job.source_worker, {{"op", "agent_cleanup"},
                                 {"job_id", job.id},
                                 {"agent_id", job.agent_id},
                                 {"name", agent_name_of(agents_, job.agent_id)}});
    }
    // exporting / verifying: awaiting a worker reply; the sweep handles
    // verifying deadlines below.
}

void MigrationService::sweep() {
    for (const auto& job : store_.inflight()) {
        if (job.state == "verifying") {
            std::int64_t deadline = 0;
            {
                std::lock_guard<std::mutex> lk(m_);
                auto it = verifying_deadline_.find(job.id);
                if (it == verifying_deadline_.end()) {
                    deadline = now_secs() + kVerifyTimeoutS;
                    verifying_deadline_[job.id] = deadline;
                } else {
                    deadline = it->second;
                }
            }
            const std::string st = hub_.agent_state(job.agent_id);
            if (st == "ready") {
                {
                    std::lock_guard<std::mutex> lk(m_);
                    verifying_deadline_.erase(job.id);
                }
                store_.set_state(job.id, "cleanup", "",
                                 "verified; archiving source");
                drive(store_.get(job.id).value_or(job));
            } else if (now_secs() > deadline) {
                {
                    std::lock_guard<std::mutex> lk(m_);
                    verifying_deadline_.erase(job.id);
                }
                if (job.no_rollback) {
                    store_.set_state(job.id, "failed",
                                     "verification timed out on target", "");
                } else {
                    rollback(job, "verification timed out");
                }
            }
            continue;
        }
        // Stale transfer across a hub restart: this process never
        // armed begun_ for it, so re-export (worker archive reuse →
        // export_ready → data_begin re-armed; target truncates at seq 0).
        Migration stepback = job;
        if (stepback.state == "transferring") {
            bool begun = false;
            {
                std::lock_guard<std::mutex> lk(m_);
                begun = begun_.count(stepback.id) > 0;
            }
            if (!begun) {
                store_.set_state(stepback.id, "exporting", "",
                                 "re-export after hub restart");
                stepback.state = "exporting";
            }
        }
        // All other live states: re-drive (idempotent per job_id).
        if (channel_.is_online(job.source_worker) &&
            channel_.is_online(job.target_worker)) {
            drive(stepback);
        }
    }
}

void MigrationService::sweep_loop() {
    while (!stop_) {
        try {
            sweep();
        } catch (const std::exception& e) {
            std::cerr << "[migration] sweep: " << e.what() << "\n";
        }
        for (std::int64_t i = 0; i < kSweepIntervalS * 10 && !stop_; ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}

}  // namespace hivemind
