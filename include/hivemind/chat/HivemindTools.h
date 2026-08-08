#pragma once

/**
 * HiveMind-control tools for the Chat agent.
 *
 * Each tool is a daedalus::BaseTool subclass that operates in-process on
 * the GraphStore (and the shared start_graph_run path). Registered on the
 * ChatAgent's daedalus::Agent at construction time.
 */

#include <daedalus.h>

#include <memory>
#include <vector>

class GraphStore;
class ExecutorRegistry;

/// Build the full HiveMind tool set. The returned tools are meant to be
/// handed to Agent::register_tool(std::move(...)) one by one.
std::vector<std::unique_ptr<daedalus::BaseTool>>
make_hivemind_tools(GraphStore& store, ExecutorRegistry& registry);
