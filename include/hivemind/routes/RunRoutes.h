#pragma once

#include <OmniNet.h>
#include <websocket/WebSocket.h>

class GraphStore;
class ExecutorRegistry;

/**
 * Register run routes:
 *   POST /api/graphs/:gid/run       — start a graph execution
 *   GET  /api/graphs/:gid/runs/:rid — poll run status
 *   WS   /ws/runs                  — WebSocket stream (subscribe by sending {"type":"subscribe","run_id":"..."})
 *
 * The WebSocket sends JSON events as the graph runs:
 *   {"event": "graph_started", "graph_id": "...", "run_id": "..."}
 *   {"event": "node_started",  "node_id": "...", "node_label": "..."}
 *   {"event": "node_output",   "node_id": "...", "chunk": "..."}
 *   {"event": "node_completed","node_id": "..."}
 *   {"event": "loop_iteration","node_id": "...", "iteration": N}
 *   {"event": "graph_completed"}
 *   {"event": "graph_failed",  "error": "..."}
 *   {"event": "run_done"}       (sent when run completes, WS closes)
 */
void register_run_routes(OmniNet::HTTP::Server& server,
                         GraphStore& store,
                         ExecutorRegistry& registry);
