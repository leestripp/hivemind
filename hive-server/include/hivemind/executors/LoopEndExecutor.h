#pragma once
#include "../NodeExecutor.h"

class LoopEndExecutor : public NodeExecutor {
public:
    std::vector<NodeResult>
    execute(const Node& node,
            const std::unordered_map<std::string, std::string>& inputs,
            const std::string& run_id) override;

    std::string_view type_name() const noexcept override { return "loop_end"; }
};
