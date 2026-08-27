#pragma once

#include <OmniNet.h>

class SystemMonitor;
class AgentStore;
class ThreadStore;

/// System page routes:
///   GET /api/system           host facts + current sample (mem/cpu/disk/…)
///   GET /api/system/history?minutes=N   ring-buffer series for graphs
///   GET /api/system/agents    per-agent usage rollup, last 24 h
///                             (exec counts/timeouts from the exec audit,
///                             message counts from threads)
void register_system_routes(OmniNet::HTTP::Server& server,
                            SystemMonitor& monitor,
                            AgentStore& agents,
                            ThreadStore& threads);
