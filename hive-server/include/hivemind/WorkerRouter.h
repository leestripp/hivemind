#pragma once

#include <optional>
#include <string>

#include <memory>

#include "hivemind/ContainerManager.h"   // ExecResult
#include "hivemind/WorkerChannel.h"      // ScreenResult

namespace OmniNet { class Socket; }

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

    /// Lightweight routing probe: true when the agent is assigned to a
    /// worker (regardless of that worker's online state — exec() itself
    /// throws on offline). False when not installed / unknown / "local".
    static bool is_remote(const std::string& agent_id);

    /// nullopt = local agent, use ContainerManager::exec. Throws
    /// std::runtime_error when the assigned worker is offline/unknown.
    static std::optional<ContainerManager::ExecResult> exec(
        const std::string& agent_id, const std::string& command,
        int timeout_s);

    /// Phase 31: nullopt = local agent, read the file from the host
    /// workspace directly. For remote agents fetches the file over the
    /// worker channel (raw bytes in FileResult::data). Throws when the
    /// assigned worker is offline.
    static std::optional<WorkerChannel::FileResult> fetch_file(
        const std::string& agent_id, const std::string& path,
        int timeout_s = 120);

    /// 30.4: nullopt = local agent, use ContainerManager screen calls.
    /// For worker agents: screen control over the channel (image +
    /// resolution passed from server config; the worker runs the same
    /// podman steps). Throws std::runtime_error when the worker is
    /// offline; ScreenResult.error set on worker-side failure.
    /// §Skills-Global: relay — list a worker's workspace global skills.
    /// nullopt = worker offline; throws on timeout.
    static std::optional<WorkerChannel::WsSkillsResult> workspace_skills(
        const std::string& worker, int timeout_s = 15);

    static std::optional<WorkerChannel::ScreenResult> screen_ctl(
        const std::string& agent_id, const std::string& action);

    /// 30.4: open a Socket facade to the agent's REMOTE screen port
    /// (ChannelSocket over the worker channel). nullptr = local agent,
    /// use a direct TCP socket. Throws when the worker is offline or the
    /// stream fails to open.
    static std::unique_ptr<OmniNet::Socket> screen_stream(
        const std::string& agent_id, uint16_t port);
};
