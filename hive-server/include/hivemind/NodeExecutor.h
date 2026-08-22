#pragma once

#include "Graph.h"
#include <string>
#include <vector>
#include <future>
#include <optional>

/**
 * NodeExecutor — base interface for executing a single node.
 *
 * Each node type has a concrete subclass (PromptExecutor, AgentExecutor, etc.)
 * The GraphRunner calls executor->execute() for each node it needs to run.
 */
class NodeExecutor {
public:
    virtual ~NodeExecutor() = default;

    /**
     * Execute this node given resolved input values.
     *
     * @param node          The node to execute (type + config_json)
     * @param inputs        Map of input_port → resolved value
     * @param run_id        Current run ID (for streaming events)
     * @return              The result of execution (output_port → value)
     *
     * Throws on fatal error. Returns NodeResult even on "soft" failures
     * (e.g., agent returned non-zero) — the caller decides whether to stop.
     */
    virtual std::vector<NodeResult>
    execute(const Node& node,
            const std::unordered_map<std::string, std::string>& inputs,
            const std::string& run_id) = 0;

    /**
     * Human-readable node type name, for logging.
     */
    virtual std::string_view type_name() const noexcept = 0;
};
