#pragma once

#include <OmniNet.h>
#include <websocket/WebSocket.h>

/**
 * Register project routes:
 *   GET /api/projects      — list all projects (directories in the projects dir)
 *   GET /api/projects/:p — list files in a project
 */
void register_project_routes(OmniNet::HTTP::Server& server);
