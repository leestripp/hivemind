#pragma once

#include <OmniNet.h>

#include "hivemind/ApprovalStore.h"

/**
 * Approval queue routes (HiveMind 2.0, Plan.md Phase 3):
 *   GET  /api/approvals?status=pending   list approvals (default: all)
 *   POST /api/approvals/:id/approve      approve a pending action
 *   POST /api/approvals/:id/deny         deny a pending action
 *
 * Phase 16 rule-based auto-review:
 *   GET  /api/agents/:id/approval-rules  ordered rule list (first match wins)
 *   PUT  /api/agents/:id/approval-rules  replace the list atomically
 *                                        [{tool, command, action}] —
 *                                        globs; action "allow"|"deny"
 */
class AgentStore;
class HubRouter;
void register_approval_routes(OmniNet::HTTP::Server& server,
                              HubRouter& hub,
                              ApprovalStore& approvals,
                              AgentStore& agents);
