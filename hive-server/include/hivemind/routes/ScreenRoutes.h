#pragma once

#include <OmniNet.h>

#include <functional>
#include <cstdint>

class AgentStore;
class AuthStore;

/**
 * Screen routes (HiveMind 2.0, Plan.md Phase 6 — computer use):
 *
 *   GET  /api/agents/:id/screen          screen session status JSON
 *   POST /api/agents/:id/screen/start    ensure browser container + display
 *                                        stack (Xvfb/x11vnc/websockify) up
 *   POST /api/agents/:id/screen/stop     stop the display stack
 *
 * Browser-facing proxy (noVNC can't send Authorization headers, so these
 * live OUTSIDE /api/* and do their own auth — Bearer, ?key= query, or an
 * HttpOnly cookie set on first ?key= access):
 *
 *   GET  /screen/:id/                    set cookie, redirect to vnc.html
 *   GET  /screen/:id/<asset>             HTTP proxy → container :6080
 *   WS   /screen/ws?agent=<id>           WebSocket splice → container
 *                                        websockify (raw TCP relay after a
 *                                        fresh upstream upgrade)
 *
 * Everything is auth-guarded at hive-server (admin or agent scope); the
 * container's :6080 is published on 127.0.0.1 only, so hive-server is the
 * single entry point — safe behind Caddy/TLS (docs/deploy.md).
 */
/// `port_resolver` overrides ContainerManager::screen_port (tests point it
/// at a dummy upstream instead of podman).
void register_screen_routes(OmniNet::HTTP::Server& server,
                            AgentStore& agents,
                            AuthStore& auth,
                            std::function<uint16_t(const std::string& agent_id)>
                                port_resolver = {});
