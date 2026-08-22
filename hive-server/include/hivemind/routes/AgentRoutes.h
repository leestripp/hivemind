#pragma once

#include <OmniNet.h>

/**
 * Register agent introspection routes:
 *   GET /api/agent/tools   — list available tools (built-in + MCP)
 *   GET /api/agent/status — agent system status
 */
void register_agent_routes(OmniNet::HTTP::Server& server);
