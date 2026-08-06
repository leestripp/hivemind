#include "hivemind/executors/MergeExecutor.h"
#include "hivemind/Graph.h"

#include <algorithm>
#include <string>
#include <vector>

std::vector<NodeResult>
MergeExecutor::execute(const Node& node,
                       const std::unordered_map<std::string, std::string>& inputs,
                       const std::string& /*run_id*/)
{
    std::string strategy = "concat";
    std::string separator = "\n\n";
    if (node.config_json.contains("strategy") && node.config_json["strategy"].is_string())
        strategy = node.config_json["strategy"].get<std::string>();
    if (node.config_json.contains("separator") && node.config_json["separator"].is_string())
        separator = node.config_json["separator"].get<std::string>();

    if (strategy != "concat")
        throw std::invalid_argument{"merge node: unknown strategy '" + strategy
                                    + "' (supported: concat)"};

    // Join in deterministic port-name order (in_a, in_b, in_c, ...).
    std::vector<std::string> keys;
    keys.reserve(inputs.size());
    for (auto& [k, _] : inputs) keys.push_back(k);
    std::sort(keys.begin(), keys.end());

    std::string merged;
    for (auto& k : keys) {
        const std::string& v = inputs.at(k);
        if (v.empty()) continue;            // skip empty upstream outputs
        if (!merged.empty()) merged += separator;
        merged += v;
    }

    return {NodeResult{node.id, "out", merged}};
}
