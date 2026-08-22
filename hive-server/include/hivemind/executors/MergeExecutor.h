#pragma once
#include "../NodeExecutor.h"

/**
 * MergeExecutor — combines multiple input ports into one output.
 *
 * v1 join strategy: concat with a configurable separator
 * (config.separator, default "\n\n"). Inputs are joined in port-name
 * order (in_a, in_b, in_c, …).
 *
 * Config schema:
 *   { "strategy": "concat", "separator": "\n\n" }
 */
class MergeExecutor : public NodeExecutor {
public:
    std::vector<NodeResult>
    execute(const Node& node,
            const std::unordered_map<std::string, std::string>& inputs,
            const std::string& run_id) override;

    std::string_view type_name() const noexcept override { return "merge"; }
};
