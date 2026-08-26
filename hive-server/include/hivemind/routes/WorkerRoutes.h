#pragma once

#include <OmniNet.h>

class WorkerStore;

/// Phase 30.1: worker registry routes.
///   GET    /api/workers         list enrolled workers (name, caps, proto,
///                               last_seen; online flag comes in 30.2)
///   DELETE /api/workers/:name   revoke a worker (key dies with the row)
/// Enrollment is CLI-only: `hive-server enroll-worker <name>`.
void register_worker_routes(OmniNet::HTTP::Server& server,
                            WorkerStore& workers);
