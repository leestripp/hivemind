#pragma once

#include <OmniNet.h>

namespace hivemind { class MigrationService; }

/// Phase 73: agent migration between workers (Plan.md). Admin-scoped.
///   POST   /api/agents/:id/migrate      {target_worker, target_type,
///                                        force?, no_rollback?, ack?}
///                                       -> 202 {job_id}
///   GET    /api/migrations              ?agent=&state= (newest first)
///   GET    /api/migrations/:id          one job
///   POST   /api/migrations/:id/cancel   safe at any live state
void register_migration_routes(OmniNet::HTTP::Server& server,
                               hivemind::MigrationService& svc);
