#pragma once

#include <OmniNet.h>

#include "hivemind/RoutineStore.h"
#include "hivemind/AgentStore.h"
#include "hivemind/ThreadStore.h"
#include "hivemind/AgentHost.h"   // ChatService

/**
 * Routine + skill routes (HiveMind 2.0, Plan.md Phase 5):
 *
 *   POST   /api/routines                 create {agent_id, name, prompt, schedule}
 *   GET    /api/routines                 list
 *   DELETE /api/routines/:id             remove
 *   POST   /api/routines/:id/run         fire immediately (off-schedule)
 *   POST   /api/routines/:id/enabled     {enabled: bool}
 *
 *   GET    /api/agents/:id/skills        list skill names for an agent
 *   POST   /api/agents/:id/skills        {name, description, content} → SKILL.md
 *   DELETE /api/agents/:id/skills/:name  remove a skill
 */
void register_routine_routes(OmniNet::HTTP::Server& server,
                             RoutineStore& routines,
                             AgentStore& agents,
                             ThreadStore& threads,
                             ChatService& chat);
