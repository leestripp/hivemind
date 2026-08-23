// =============================================================================
// src/ContainerTools.cpp — container-backed agent tools (HiveMind 2.0 Phase 2)
// =============================================================================
#include "hivemind/ContainerTools.h"
#include "hivemind/ContainerManager.h"

nlohmann::json ContainerBashTool::execute(const nlohmann::json& args,
                                          daedalus::HostToolContext& /*ctx*/) {
    std::string command = args.value("command", "");
    if (command.empty())
        return {{"status", "error"}, {"error", "missing 'command' argument"}};

    int timeout = args.value("timeout", 120);
    if (timeout <= 0 || timeout > 3600) timeout = 120;

    try {
        auto res = container_manager().exec(agent_id_, command, timeout);
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
