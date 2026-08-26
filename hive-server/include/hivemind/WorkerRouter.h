#pragma once

#include <optional>
#include <string>

#include "hivemind/ContainerManager.h"   // ExecResult

class AgentStore;
class WorkerChannel;

/**
 * WorkerRouter — Phase 30.3: container_exec dispatcher.
 *
 * Decides WHERE an agent's command runs by its roster `worker` field:
 *   - "local"  → returns nullopt; the caller takes today's
 *                ContainerManager::exec path unchanged.
 *   - <name>   → synchronous exec over the hive-worker channel
 *                (WorkerChannel::exec). Secrets are resolved SERVER-side
 *                (SecretStore::env_for_agent) and sent as rendered
 *                env-file content; the worker never sees the secrets DB.
 *                The audit row is written here with the worker name.
 *
 * Fail-fast per design: a worker-assigned agent whose worker is OFFLINE
 * throws — no silent local fallback (that would run the workload on the
 * wrong machine).
 *
 * Process-wide static (matches ContainerManager::secret_store() style):
 * installed once from main() after the stores/channel exist.
 */
class WorkerRouter {
public:
    static void install(AgentStore* agents, WorkerChannel* channel);

    /// nullopt = local agent, use ContainerManager::exec. Throws
    /// std::runtime_error when the assigned worker is offline/unknown.
    static std::optional<ContainerManager::ExecResult> exec(
        const std::string& agent_id, const std::string& command,
        int timeout_s);
};
