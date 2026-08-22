#include "hivemind/executors/LogExecutor.h"
#include "hivemind/Graph.h"
#include <iostream>

std::vector<NodeResult>
LogExecutor::execute(const Node& node,
                      const std::unordered_map<std::string, std::string>& inputs,
                      const std::string& /*run_id*/)
{
    std::ostringstream ss;
    ss << "[log:" << node.label << "] ";
    for (auto& [port, val] : inputs) {
        if (!ss.str().ends_with("] ")) ss << " ";
        ss << "[" << port << "] " << val;
    }
    std::cout << ss.str() << "\n";
    return {};
}
