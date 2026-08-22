#include "hivemind/executors/PromptExecutor.h"
#include "hivemind/Graph.h"
#include <string>

std::vector<NodeResult>
PromptExecutor::execute(const Node& node,
                        const std::unordered_map<std::string, std::string>& /*inputs*/,
                        const std::string& /*run_id*/)
{
    std::string value;
    if (node.config_json.contains("value") && node.config_json["value"].is_string())
        value = node.config_json["value"].get<std::string>();

    return {NodeResult{node.id, "out", value}};
}
