#pragma once

#include <string>
#include <string_view>

namespace OmniNet::HTTP { class Server; }

/// WebSocket endpoint /ws/runs — live run-event streaming.
///
/// Clients connect, then send {"type":"subscribe","run_id":"<id>"} (may
/// repeat for multiple runs). The server replies {"type":"subscribed",…},
/// backfills any buffered events for that run, then streams each new
/// event as one WS text message (same JSON shape as the polling buffer).
void register_ws_run_routes(OmniNet::HTTP::Server& server);

/// Fan-out hook called by RunRoutes::append_run_output() for every
/// buffered event line (without the trailing newline).
void ws_run_publish(const std::string& run_id, std::string_view event_json);
