#pragma once
#include "hivemind/NodeExecutor.h"
/// agent_loop_start: entry of an agent-judged loop. Passthrough; emits
/// loop_count = config.max_iterations (safety cap, default 10). On later
/// iterations the runner injects the judge agent's feedback into "in".
class AgentLoopStartExecutor final : public NodeExecutor {
public:
    std::vector<NodeResult> execute(const Node& node,
        const std::unordered_map<std::string, std::string>& inputs,
        const std::string& run_id) override;
    std::string_view type_name() const noexcept override { return "agent_loop_start"; }
};
