#pragma once

#include <OmniNet.h>

class SystemMonitor;
class WorkerTelemetry;
class WorkerStore;
class WorkerChannel;
class AgentStore;
class ThreadStore;

/// System page routes:
///   GET /api/system           hub fallback + per-worker host/current samples
///   GET /api/system/history?minutes=N[&worker=name]  hub or worker series
///   GET /api/system/agents    per-agent usage rollup, last 24 h
///                             (exec counts/timeouts from the exec audit,
///                             message counts from threads)
void register_system_routes(OmniNet::HTTP::Server& server,
                            SystemMonitor& monitor,
                            WorkerTelemetry& telemetry,
                            WorkerStore& workers,
                            WorkerChannel& channel,
                            AgentStore& agents,
                            ThreadStore& threads);
