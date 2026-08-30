// include/hivemind/routes/ApprovalRoutes.cpp — approval queue REST API.
#include "hivemind/routes/RouteUtil.h"
#include "hivemind/HubRouter.h"
#include "hivemind/routes/ApprovalRoutes.h"
#include "hivemind/AgentStore.h"

#include <nlohmann/json.hpp>

#include <string>
#include <vector>

using json = nlohmann::json;

namespace {
using hivemind::routes::path_segments;
using hivemind::routes::send_json;
using hivemind::routes::send_error;






json approval_json(const Approval& a) {
    return {
        {"id", a.id}, {"agent_id", a.agent_id}, {"agent_name", a.agent_name},
        {"kind", a.kind}, {"payload", a.payload}, {"status", a.status},
        {"created_at", a.created_at}, {"resolved_at", a.resolved_at},
    };
}

} // namespace

void register_approval_routes(OmniNet::HTTP::Server& server,
                              HubRouter& hub,
                              ApprovalStore& approvals,
                              AgentStore& agents) {

    server.get("/api/approvals",
        [&approvals](const OmniNet::HTTP::Request& req,
                     OmniNet::HTTP::Response& resp) {
            std::string status = req.queryParameter("status", "");
            json list = json::array();
            for (auto& a : approvals.list(status)) list.push_back(approval_json(a));
            send_json(resp, json{{"approvals", list}});
        });

    auto resolve = [&approvals, &hub](const OmniNet::HTTP::Request& req,
                                OmniNet::HTTP::Response& resp, bool approved) {
        auto seg = path_segments(req.path());   // api approvals :id approve
        if (seg.size() < 4) { send_json(resp, json{{"error", "bad path"}}, 400); return; }
        // Phase C: hub-owned approvals relay approval_resp to the worker
        // (hub.resolve_approval resolves the store row too). Plain store
        // resolve remains as the fallback for non-worker rows.
        if (!hub.resolve_approval(seg[2], approved) &&
            !approvals.resolve(seg[2], approved)) {
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

    // ── Phase 16: per-agent auto-review rules ───────────────────────────
    auto rules_agent = [&agents](const OmniNet::HTTP::Request& req,
                                 OmniNet::HTTP::Response& resp) -> std::string {
        auto seg = path_segments(req.path());  // api agents :id approval-rules
        if (seg.size() < 4 || seg[0] != "api" || seg[1] != "agents" ||
            seg[3] != "approval-rules" || !agents.get_agent(seg[2])) {
            send_json(resp, json{{"error", "agent not found"}}, 404);
            return {};
        }
        return seg[2];
    };

    server.get("/api/agents/:id/approval-rules",
        [&approvals, rules_agent](const OmniNet::HTTP::Request& req,
                                  OmniNet::HTTP::Response& resp) {
            const std::string id = rules_agent(req, resp);
            if (id.empty()) return;
            json list = json::array();
            for (auto& r : approvals.list_rules(id))
                list.push_back({{"tool", r.tool_pattern},
                                {"command", r.command_pattern},
                                {"action", r.action}});
            send_json(resp, json{{"rules", list}});
        });

    server.put("/api/agents/:id/approval-rules",
        [&approvals, rules_agent](const OmniNet::HTTP::Request& req,
                                  OmniNet::HTTP::Response& resp) {
            const std::string id = rules_agent(req, resp);
            if (id.empty()) return;
            json body;
            try { body = json::parse(req.bodyAsString()); }
            catch (...) { send_json(resp, json{{"error", "invalid JSON"}}, 400); return; }
            if (!body.contains("rules") || !body["rules"].is_array()) {
                send_json(resp, json{{"error", "rules must be an array"}}, 400);
                return;
            }
            std::vector<ApprovalRule> rules;
            for (auto& j : body["rules"]) {
                ApprovalRule r;
                r.tool_pattern = j.value("tool", "");
                r.command_pattern = j.value("command", "");
                r.action = j.value("action", "");
                if (r.tool_pattern.empty() || r.command_pattern.empty()) {
                    send_json(resp, json{{"error", "tool and command patterns must be non-empty"}}, 400);
                    return;
                }
                rules.push_back(std::move(r));
            }
            try {
                approvals.replace_rules(id, rules);
            } catch (const std::invalid_argument& e) {
                send_json(resp, json{{"error", e.what()}}, 400); return;
            }
            send_json(resp, json{{"saved", true}, {"count", rules.size()}});
        });
}
