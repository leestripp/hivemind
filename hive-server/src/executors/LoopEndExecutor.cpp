#include "hivemind/executors/LoopEndExecutor.h"
#include "hivemind/Graph.h"

std::vector<NodeResult>
LoopEndExecutor::execute(const Node& node,
                           const std::unordered_map<std::string, std::string>& inputs,
                           const std::string& /*run_id*/)
{
    // Pass through the body text. The GraphRunner decides whether this is the
    // final iteration (and the output propagates) or we should jump back.
    std::string text = inputs.count("in") ? inputs.at("in") : std::string{};
    return {NodeResult{node.id, "out", text}};
}
