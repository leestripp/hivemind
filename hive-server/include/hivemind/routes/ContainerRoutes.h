#pragma once

#include "hivemind/AgentStore.h"
#include <OmniNet.h>

/**
 * ContainerRoutes (v2): workers own podman; container lifecycle is implicit
 * via agent_spawn/agent_remove. This file keeps only the admin surface:
 *   POST /api/agents/:id/container/exec   {command, timeout} - channel-routed
 *   GET  /api/agents/:id/exec-log         audit trail (newest first)
 */
void register_container_routes(OmniNet::HTTP::Server& server,
                               AgentStore& agents);
