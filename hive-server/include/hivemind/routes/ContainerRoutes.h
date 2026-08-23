#pragma once

#include <OmniNet.h>

#include "hivemind/AgentStore.h"

/**
 * Container lifecycle routes (HiveMind 2.0, Plan.md Phase 2):
 *
 *   GET  /api/agents/:id/container/status   {exists, running, container, image}
 *   POST /api/agents/:id/container/start    create+start (idempotent)
 *   POST /api/agents/:id/container/stop     stop (container kept)
 *   POST /api/agents/:id/container/exec     {command, timeout?} — admin/debug
 */
void register_container_routes(OmniNet::HTTP::Server& server, AgentStore& agents);
