// [FILTERED_API_KEY]
// src/executors/OutputExecutor.cpp
// -------------------------------------------------------------------------
// `output` node: collects a named value into the run's result payload.
//
// The key used in `GraphRun.results` is the node's configured `output_name`
// (falling back to `name` for older graphs). We store it as the `node_id`
// column of `run_results` with an empty port so that `GraphStore::get_run`
// can surface it as `results["output_name"] = value`.
// [FILTERED_API_KEY]

#include "hivemind/executors/OutputExecutor.h"
#include "hivemind/Graph.h"

#include <stdexcept>
#include <string>

std::vector<NodeResult>
OutputExecutor::execute(const Node& node,
                        const std::unordered_map<std::string, std::string>& inputs,
                        const std::string& /*run_id*/)
{
    std::string output_name;
    if (node.config_json.contains("output_name")
        && node.config_json["output_name"].is_string()) {
        output_name = node.config_json["output_name"].get<std::string>();
    } else if (node.config_json.contains("name")
               && node.config_json["name"].is_string()) {
        output_name = node.config_json["name"].get<std::string>();
    }

    if (output_name.empty()) {
        throw std::invalid_argument{
            "output node '" + node.label + "' (" + node.id +
            ") is missing config.output_name (or config.name)"};
    }

    // Collect the first (and only) input value.
    std::string value;
    for (auto& [k, v] : inputs) { value = v; break; }

    // Store under the output name as the public result key. The empty port
    // tells GraphStore::get_run() to omit the ":<port>" suffix.
    return {NodeResult{output_name, "", value}};
}
