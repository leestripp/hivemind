#pragma once
#include "hivemind/NodeExecutor.h"
/// agent_loop_end: judge of an agent-judged loop. Runs a daedalus agent
/// with a single `loop_decision` tool ({decision:"continue"|"exit",
/// feedback}). "continue" → returns sentinel port "__continue__" carrying
/// the feedback (runner jumps back). "exit" → returns "out" (final text).
class AgentLoopEndExecutor final : public NodeExecutor {
public:
    std::vector<NodeResult> execute(const Node& node,
        const std::unordered_map<std::string, std::string>& inputs,
        const std::string& run_id) override;
    std::string_view type_name() const noexcept override { return "agent_loop_end"; }
};
