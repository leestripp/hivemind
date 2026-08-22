#include "hivemind/executors/InputExecutor.h"
#include "hivemind/Graph.h"
#include <stdexcept>

std::vector<NodeResult>
InputExecutor::execute(const Node& node,
                        const std::unordered_map<std::string, std::string>& inputs,
                        const std::string& /*run_id*/)
{
    // GraphRunner seeds resolved_["<id>:out"] from the run payload
    // (keyed by config.param_name) and injects it into inputs["out"] before
    // calling us — pass it through. Falling back to a static config value
    // keeps the node usable in graphs that don't take run inputs.
    auto it = inputs.find("out");
    if (it != inputs.end()) return {NodeResult{node.id, "out", it->second}};
    for (const char* key : {"value", "default_value"}) {
        if (node.config_json.contains(key) && node.config_json[key].is_string())
            return {NodeResult{node.id, "out", node.config_json[key].get<std::string>()}};
    }
    return {NodeResult{node.id, "out", ""}};
}
