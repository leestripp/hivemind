#pragma once

#include <OmniNet.h>
#include <websocket/WebSocket.h>

class GraphStore;

/**
 * Register REST API routes for graph CRUD.
 */
void register_graph_routes(OmniNet::HTTP::Server& server, GraphStore& store);

/**
 * Register run routes: POST /api/graphs/:id/run and polling GET.
 */
void register_run_routes(OmniNet::HTTP::Server& server,
                         GraphStore& store,
                         class ExecutorRegistry& registry);

/**
 * Register model listing route: GET /api/models.
 */
void register_model_routes(OmniNet::HTTP::Server& server);

/// Portable workflow files: export / import (.hivemind.json).
void register_workflow_routes(OmniNet::HTTP::Server& server, GraphStore& store);
