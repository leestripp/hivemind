// include/hivemind/routes/ApprovalRoutes.cpp — approval queue REST API.
#include "hivemind/routes/ApprovalRoutes.h"

#include <nlohmann/json.hpp>

#include <string>
#include <vector>

using json = nlohmann::json;

namespace {

std::vector<std::string> path_segments(std::string_view path) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : path) {
        if (c == '/') { if (!cur.empty()) { out.push_back(cur); cur.clear(); } }
        else cur += c;
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

void send_json(OmniNet::HTTP::Response& resp, const json& j, int status = 200) {
    resp = OmniNet::HTTP::Response::ok();
    resp.setStatus(static_cast<OmniNet::HTTP::StatusCode>(status));
    resp.setContentType("application/json");
    resp.setBody(j.dump());
}

json approval_json(const Approval& a) {
    return {
        {"id", a.id}, {"agent_id", a.agent_id}, {"agent_name", a.agent_name},
        {"kind", a.kind}, {"payload", a.payload}, {"status", a.status},
        {"created_at", a.created_at}, {"resolved_at", a.resolved_at},
    };
}

} // namespace

void register_approval_routes(OmniNet::HTTP::Server& server,
                              ApprovalStore& approvals) {

    server.get("/api/approvals",
        [&approvals](const OmniNet::HTTP::Request& req,
                     OmniNet::HTTP::Response& resp) {
            std::string status = req.queryParameter("status", "");
            json list = json::array();
            for (auto& a : approvals.list(status)) list.push_back(approval_json(a));
            send_json(resp, json{{"approvals", list}});
        });

    auto resolve = [&approvals](const OmniNet::HTTP::Request& req,
                                OmniNet::HTTP::Response& resp, bool approved) {
        auto seg = path_segments(req.path());   // api approvals :id approve
        if (seg.size() < 4) { send_json(resp, json{{"error", "bad path"}}, 400); return; }
        if (!approvals.resolve(seg[2], approved)) {
            send_json(resp, json{{"error", "not found or already resolved"}}, 404);
            return;
        }
        send_json(resp, json{{"id", seg[2]},
                             {"status", approved ? "approved" : "denied"}});
    };

    server.post("/api/approvals/:id/approve",
        [resolve](const OmniNet::HTTP::Request& req,
                  OmniNet::HTTP::Response& resp) { resolve(req, resp, true); });
    server.post("/api/approvals/:id/deny",
        [resolve](const OmniNet::HTTP::Request& req,
                  OmniNet::HTTP::Response& resp) { resolve(req, resp, false); });
}
