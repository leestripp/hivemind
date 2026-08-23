#pragma once

#include <OmniNet.h>

#include "hivemind/AgentStore.h"
#include "hivemind/ThreadStore.h"
#include "hivemind/AgentHost.h"

/**
 * Roster + chat routes (HiveMind 2.0, Plan.md Phase 1):
 *
 *   POST   /api/agents                    create named agent
 *   GET    /api/agents                    list roster
 *   GET    /api/agents/:id                get one
 *   PATCH  /api/agents/:id                update name/role/model/workspace_mode
 *   DELETE /api/agents/:id                remove from roster
 *
 *   POST   /api/threads                   create thread {title?, agent_ids[]}
 *   GET    /api/threads                   list threads (newest first)
 *   GET    /api/threads/:id               thread + participants
 *   DELETE /api/threads/:id               delete thread + messages
 *   GET    /api/threads/:id/messages?after=<msg_id>   messages (incremental)
 *   POST   /api/threads/:id/messages      {body} → user message + agent turns
 *   GET    /api/threads/:id/activity      in-progress agent turns (streaming)
 */
void register_roster_routes(OmniNet::HTTP::Server& server,
                            AgentStore& agents,
                            ThreadStore& threads,
                            AgentRegistry& registry,
                            ChatService& chat);
