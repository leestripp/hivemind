#pragma once

#include <OmniNet.h>

#include "hivemind/ApprovalStore.h"

/**
 * Approval queue routes (HiveMind 2.0, Plan.md Phase 3):
 *   GET  /api/approvals?status=pending   list approvals (default: all)
 *   POST /api/approvals/:id/approve      approve a pending action
 *   POST /api/approvals/:id/deny         deny a pending action
 */
void register_approval_routes(OmniNet::HTTP::Server& server,
                              ApprovalStore& approvals);
