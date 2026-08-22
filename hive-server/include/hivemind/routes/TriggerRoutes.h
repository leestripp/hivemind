#pragma once

#include <OmniNet.h>

class GraphStore;
class ExecutorRegistry;

/**
 * Trigger routes — auto-fire workflows from external events.
 *
 *   POST   /api/graphs/:gid/triggers   create a trigger (v1: webhook only)
 *   GET    /api/graphs/:gid/triggers   list triggers for a graph
 *   DELETE /api/triggers/:id           delete a trigger
 *   POST   /api/hooks/:gid/:secret     fire a webhook trigger → starts a run
 *
 * Webhook body mapping to run inputs: a JSON object becomes inputs verbatim
 * (string values used as-is, others dump()'d); any other body is passed as
 * inputs["body"].
 */
void register_trigger_routes(OmniNet::HTTP::Server& server,
                             GraphStore& store,
                             ExecutorRegistry& registry);
