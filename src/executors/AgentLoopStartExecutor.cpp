#include "hivemind/executors/AgentLoopStartExecutor.h"
#include "hivemind/Graph.h"

std::vector<NodeResult>
AgentLoopStartExecutor::execute(const Node& node,
    const std::unordered_map<std::string, std::string>& inputs,
    const std::string& /*run_id*/)
{
    int max_iter = 10;
    if (node.config_json.contains("max_iterations")) {
        auto& v = node.config_json["max_iterations"];
        if (v.is_number_integer()) max_iter = v.get<int>();
        else if (v.is_string()) try { max_iter = std::stoi(v.get<std::string>()); } catch (...) {}
    }
    if (max_iter < 1) max_iter = 1;
    std::string text = inputs.count("in") ? inputs.at("in") : std::string{};
    return {
        NodeResult{node.id, "out", text},
        NodeResult{node.id, "loop_count", std::to_string(max_iter)},
    };
}
