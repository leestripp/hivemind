#pragma once

// =============================================================================
// RouteUtil.h — shared helpers for route registrars (HiveMind 2.0, Phase 10
// consolidation). Previously copy-pasted into every *Routes.cpp.
// =============================================================================

#include <OmniNet.h>
#include <nlohmann/json.hpp>

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

inline void send_json(OmniNet::HTTP::Response& resp, const nlohmann::json& j,
                      int status = 200) {
    resp = OmniNet::HTTP::Response::ok();
    resp.setStatus(static_cast<OmniNet::HTTP::StatusCode>(status));
    resp.setContentType("application/json");
    resp.setBody(j.dump());
}

inline void send_error(OmniNet::HTTP::Response& resp, int status,
                       const std::string& msg) {
    send_json(resp, nlohmann::json{{"error", msg}}, status);
}

} // namespace hivemind::routes
