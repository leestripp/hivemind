// include/hivemind/routes/ContainerRoutes.cpp
// -----------------------------------------------------------------------------
// Container lifecycle REST API (HiveMind 2.0, Plan.md Phase 2).
// Multi-param path segments parsed manually (OmniNet pathParam caveat).
// -----------------------------------------------------------------------------

#include "hivemind/routes/RouteUtil.h"
#include "hivemind/routes/ContainerRoutes.h"
#include "hivemind/ContainerManager.h"

#include <nlohmann/json.hpp>

#include <string>
#include <vector>

using json = nlohmann::json;

namespace {
using hivemind::routes::path_segments;
using hivemind::routes::send_json;
using hivemind::routes::send_error;








} // namespace

void register_container_routes(OmniNet::HTTP::Server& server, AgentStore& agents) {

    // /api/agents/:id/container/{status,start,stop,exec}
    // segments: api(0) agents(1) :id(2) container(3) action(4)

    server.get("/api/agents/:id/container/status",
        [&agents](const OmniNet::HTTP::Request& req, OmniNet::HTTP::Response& resp) {
            auto seg = path_segments(req.path());
            if (seg.size() < 5) { send_error(resp, 400, "bad path"); return; }
            if (!agents.get_agent(seg[2])) { send_error(resp, 404, "agent not found"); return; }
            auto st = container_manager().status(seg[2]);
            send_json(resp, json{
                {"exists", st.exists}, {"running", st.running},
                {"container", st.container}, {"image", st.image},
                {"workspace", st.workspace},
                {"podman_available", ContainerManager::podman_available()},
            });
        });

    server.post("/api/agents/:id/container/start",
        [&agents](const OmniNet::HTTP::Request& req, OmniNet::HTTP::Response& resp) {
            auto seg = path_segments(req.path());
            if (seg.size() < 5) { send_error(resp, 400, "bad path"); return; }
            if (!agents.get_agent(seg[2])) { send_error(resp, 404, "agent not found"); return; }
            try {
                std::string cname = container_manager().ensure_running(seg[2]);
                send_json(resp, json{{"container", cname}, {"running", true}});
            } catch (const std::exception& e) {
                send_error(resp, 500, e.what());
            }
        });

    server.post("/api/agents/:id/container/stop",
        [&agents](const OmniNet::HTTP::Request& req, OmniNet::HTTP::Response& resp) {
            auto seg = path_segments(req.path());
            if (seg.size() < 5) { send_error(resp, 400, "bad path"); return; }
            if (!agents.get_agent(seg[2])) { send_error(resp, 404, "agent not found"); return; }
            bool ok = container_manager().stop(seg[2]);
            send_json(resp, json{{"stopped", ok}}, ok ? 200 : 500);
        });

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
            auto res = container_manager().exec(seg[2], command, timeout);
            send_json(resp, json{
                {"exit_code", res.exit_code}, {"output", res.output},
                {"timed_out", res.timed_out},
            });
        });
}
