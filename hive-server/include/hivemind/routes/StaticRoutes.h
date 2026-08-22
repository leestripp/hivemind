#pragma once

#include <OmniNet.h>
#include <websocket/WebSocket.h>

/**
 * Register static file serving routes:
 *   GET /          → web/dist/index.html  (or fallback)
 *   GET /app.js    → web/dist/app.js
 *   GET /styles.css → web/dist/styles.css
 */
void register_static_routes(OmniNet::HTTP::Server& server);
