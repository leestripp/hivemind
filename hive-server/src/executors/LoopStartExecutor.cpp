#include "hivemind/executors/LoopStartExecutor.h"
#include "hivemind/Graph.h"

#include <stdexcept>

std::vector<NodeResult>
LoopStartExecutor::execute(const Node& node,
                            const std::unordered_map<std::string, std::string>& inputs,
                            const std::string& /*run_id*/)
{
    int loop_count = 1;
    if (node.config_json.contains("loop_count")) {
        if (node.config_json["loop_count"].is_number_integer()) {
            loop_count = node.config_json["loop_count"].get<int>();
        } else if (node.config_json["loop_count"].is_string()) {
            loop_count = std::stoi(node.config_json["loop_count"].get<std::string>());
        }
    }
    if (loop_count < 1) loop_count = 1;

    std::string text = inputs.count("in") ? inputs.at("in") : std::string{};

    return {
        NodeResult{node.id, "out", text},
        NodeResult{node.id, "loop_count", std::to_string(loop_count)}
    };
}
