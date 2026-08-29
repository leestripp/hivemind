#pragma once

// =============================================================================
// FileRoutes.h — Phase 18: chat attachments + shared-workspace files API.
//
//   POST /api/threads/:id/attachments?name=<fn>  raw body → saved under
//                                                <workspace>/attachments/:id/
//   GET  /api/files?path=<rel>                   list a workspace directory
//   GET  /api/files/download?path=<rel>          download a workspace file
//
// Attachments land in the shared company workspace, so agent containers see
// them at /workspace/attachments/:id/<fn> with no copy step. The files API
// is the artifacts browser backend: agents write outputs into /workspace and
// the web UI / client browse them here. All paths are confined to the
// workspace root via hm::files::resolve_confined.
// =============================================================================

#include <OmniNet.h>

class ThreadStore;
class AgentStore;
class WorkerChannel;

void register_file_routes(OmniNet::HTTP::Server& server,
                          ThreadStore& threads,
                          AgentStore& agents,          // Phase 33 D
                          WorkerChannel* wchannel);    // Phase 33 D (nullable)
