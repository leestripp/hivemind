// include/hivemind/routes/ContainerRoutes.cpp
// -----------------------------------------------------------------------------
// Container REST API (v2): workers own podman. Container lifecycle is implicit
// (agent_spawn / agent_remove) - the only admin surface kept is a channel-routed
// exec for debugging, plus the exec audit trail.
// Multi-param path segments parsed manually (OmniNet pathParam caveat).
// -----------------------------------------------------------------------------

#include "hivemind/routes/RouteUtil.h"
#include "hivemind/routes/ContainerRoutes.h"
#include "hivemind/ContainerManager.h"   // exec_audit() static
#include "hivemind/WorkerRouter.h"
#include "hivemind/ExecAuditStore.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <string>
#include <vector>

using json = nlohmann::json;

namespace {
using hivemind::routes::path_segments;
using hivemind::routes::send_json;
using hivemind::routes::send_error;
} // namespace

void register_container_routes(OmniNet::HTTP::Server& server, AgentStore& agents) {

    // /api/agents/:id/container/exec - admin shell via the owning worker.
    // segments: api(0) agents(1) :id(2) container(3) exec(4)
    server.post("/api/agents/:id/container/exec",
        [&agents](const OmniNet::HTTP::Request& req, OmniNet::HTTP::Response& resp) {
            auto seg = path_segments(req.path());
            if (seg.size() < 5) { send_error(resp, 400, "bad path"); return; }
            if (!agents.get_agent(seg[2])) { send_error(resp, 404, "agent not found"); return; }
            json body;
            try { body = json::parse(req.bodyAsString()); }
            catch (...) { send_error(resp, 400, "invalid JSON"); return; }
            std::string command = body.value("command", "");
            if (command.empty()) { send_error(resp, 400, "command required"); return; }
            int timeout = body.value("timeout", 120);
            try {
                auto remote = WorkerRouter::exec(seg[2], command, timeout);
                if (!remote) { send_error(resp, 503, "agent's worker is offline"); return; }
                send_json(resp, json{
                    {"exit_code", remote->exit_code}, {"output", remote->output},
                    {"timed_out", remote->timed_out},
                });
            } catch (const std::exception& e) {
                send_error(resp, 503, e.what());
            }
        });

    // exec audit trail - newest first.
    server.get("/api/agents/:id/exec-log",
        [&agents](const OmniNet::HTTP::Request& req, OmniNet::HTTP::Response& resp) {
            auto seg = path_segments(req.path());
            if (seg.size() < 4) { send_error(resp, 400, "bad path"); return; }
            if (!agents.get_agent(seg[2])) { send_error(resp, 404, "agent not found"); return; }
            int limit = 100;
            try { limit = std::min(500, std::max(1,
                std::stoi(req.queryParameter("limit", "100")))); }
            catch (...) {}
            json entries = json::array();
            for (const auto& e : ContainerManager::exec_audit().list(seg[2], limit))
                entries.push_back({
                    {"id", e.id}, {"ts", e.ts}, {"command", e.command},
                    {"exit_code", e.exit_code}, {"timed_out", e.timed_out},
                    {"duration_ms", e.duration_ms}, {"worker", e.worker},
                });
            send_json(resp, json{{"entries", entries}});
        });
}
