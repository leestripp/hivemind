// src/WorkerRouter.cpp — Phase 30.3: container_exec dispatcher.
// See WorkerRouter.h for the model.

#include "hivemind/WorkerRouter.h"
#include "hivemind/AgentStore.h"
#include "hivemind/WorkerChannel.h"
#include "hivemind/SecretStore.h"
#include "hivemind/ExecAuditStore.h"

#include <chrono>
#include <stdexcept>

namespace {
AgentStore* g_agents = nullptr;
WorkerChannel* g_channel = nullptr;
}

void WorkerRouter::install(AgentStore* agents, WorkerChannel* channel) {
    g_agents = agents;
    g_channel = channel;
}

std::optional<ContainerManager::ExecResult> WorkerRouter::exec(
        const std::string& agent_id, const std::string& command,
        int timeout_s) {
    if (!g_agents || !g_channel) return std::nullopt;   // not installed
    auto agent = g_agents->get_agent(agent_id);
    if (!agent || agent->worker == "local") return std::nullopt;

    const std::string& wname = agent->worker;
    if (!g_channel->is_online(wname))
        throw std::runtime_error("worker '" + wname + "' is offline");

    // Render the env-file content SERVER-side (granted secrets only);
    // the worker gets text for a 0600 tmpfs file, never the secrets DB.
    std::string env_content;
    for (auto& [k, v] : ContainerManager::secret_store().env_for_agent(agent_id))
        env_content += k + "=" + v + "\n";

    const auto t0 = std::chrono::steady_clock::now();
    auto r = g_channel->exec(wname, agent_id, command, env_content, timeout_s);
    if (!r)
        throw std::runtime_error("worker '" + wname +
                                 "' went offline mid-exec");
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0).count();

    ContainerManager::ExecResult res;
    res.exit_code = r->exit_code;
    res.output = std::move(r->output);
    res.timed_out = r->timed_out;
    try {
        ContainerManager::exec_audit().log(agent_id, command, res.exit_code,
                                           res.timed_out, ms, wname);
    } catch (...) {}
    return res;
}
