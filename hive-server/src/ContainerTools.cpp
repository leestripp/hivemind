// =============================================================================
// src/ContainerTools.cpp — container-backed agent tools (HiveMind 2.0 Phase 2)
// =============================================================================
#include "hivemind/ContainerTools.h"
#include "hivemind/ContainerManager.h"
#include "hivemind/WorkerRouter.h"

nlohmann::json ContainerBashTool::execute(const nlohmann::json& args,
                                          daedalus::HostToolContext& /*ctx*/) {
    std::string command = args.value("command", "");
    if (command.empty())
        return {{"status", "error"}, {"error", "missing 'command' argument"}};

    int timeout = args.value("timeout", 120);
    if (timeout <= 0 || timeout > 3600) timeout = 120;

    // Phase 3 approval gate (host tools bypass daedalus' own command
    // approval, so the container tool enforces it itself).
    if (approval_fn_ && !approval_fn_(command))
        return {{"status", "denied"},
                {"error", "command was not approved by the user"}};

    try {
        // Phase 30.3: worker-assigned agents exec on their hive-worker.
        // nullopt = local agent → today's ContainerManager path.
        auto remote = WorkerRouter::exec(agent_id_, command, timeout);
        auto res = remote ? std::move(*remote)
                          : container_manager().exec(agent_id_, command, timeout);
        nlohmann::json out{
            {"exit_code", res.exit_code},
            {"output", res.output},
            {"timed_out", res.timed_out},
            {"status", res.exit_code == 0 ? "success" : "error"},
        };
        return out;
    } catch (const std::exception& e) {
        return {{"status", "error"},
                {"error", std::string{"container exec failed: "} + e.what()}};
    }
}
