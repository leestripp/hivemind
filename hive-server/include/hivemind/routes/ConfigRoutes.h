#pragma once

#include <OmniNet.h>

/**
 * Server config routes (HiveMind 2.0, workspace config):
 *
 *   GET /api/config          current config (any valid key)
 *   PUT /api/config          update — admin scope only (auth middleware).
 *                            {workspace_root} — host folder bind-mounted at
 *                            /workspace in every agent container. Applies to
 *                            containers created after the change.
 */
void register_config_routes(OmniNet::HTTP::Server& server);
