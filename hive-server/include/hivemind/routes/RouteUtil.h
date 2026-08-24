#pragma once

// =============================================================================
// RouteUtil.h — shared helpers for route registrars (HiveMind 2.0, Phase 10
// consolidation). Previously copy-pasted into every *Routes.cpp.
// =============================================================================

#include <OmniNet.h>
#include <nlohmann/json.hpp>

#include "hivemind/ServerConfig.h"

#include <string>
#include <string_view>
#include <vector>

namespace hivemind::routes {

/// Split a request path into positional segments ("/api/agents/x/y" →
/// {"api","agents","x","y"}). Use this instead of OmniNet pathParam(i),
/// which is unreliable for multi-param routes (see AGENTS.md rule 6).
inline std::vector<std::string> path_segments(std::string_view path) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : path) {
        if (c == '/') { if (!cur.empty()) { out.push_back(cur); cur.clear(); } }
        else cur += c;
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

/// Phase 9 CORS: OmniNet middleware runs BEFORE the handler and send_json
/// reassigns `resp`, wiping any headers a middleware set. Handoff instead:
/// main.cpp's first middleware stashes the request's Origin here (same
/// thread — OmniNet dispatch is synchronous), and send_json applies the
/// CORS headers when the origin matches ServerConfig's allowlist.
inline thread_local std::string request_origin;

inline void apply_cors(OmniNet::HTTP::Response& resp) {
    if (request_origin.empty()) return;
    if (!ServerConfig::instance().cors_allowed(request_origin)) return;
    resp.setHeader("Access-Control-Allow-Origin", request_origin);
    resp.setHeader("Vary", "Origin");
}

inline void send_json(OmniNet::HTTP::Response& resp, const nlohmann::json& j,
                      int status = 200) {
    resp = OmniNet::HTTP::Response::ok();
    resp.setStatus(static_cast<OmniNet::HTTP::StatusCode>(status));
    resp.setContentType("application/json");
    resp.setBody(j.dump());
    apply_cors(resp);
}

inline void send_error(OmniNet::HTTP::Response& resp, int status,
                       const std::string& msg) {
    send_json(resp, nlohmann::json{{"error", msg}}, status);
}

} // namespace hivemind::routes
