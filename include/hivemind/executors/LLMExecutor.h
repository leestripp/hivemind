#pragma once
#include "../NodeExecutor.h"

// LLMExecutor — single text→text LLM call.
// Takes: model (name from settings), prompt (from input).
// Returns: text output from the LLM.
class LLMExecutor : public NodeExecutor {
public:
    std::vector<NodeResult>
    execute(const Node& node,
            const std::unordered_map<std::string, std::string>& inputs,
            const std::string& run_id) override;

    std::string_view type_name() const noexcept override { return "llm"; }
};
