// include/hivemind/routes/ConfigRoutes.cpp — server config REST API.
#include "hivemind/routes/RouteUtil.h"
#include "hivemind/routes/ConfigRoutes.h"
#include "hivemind/ServerConfig.h"

#include <nlohmann/json.hpp>

#include <string>

using json = nlohmann::json;

namespace {
using hivemind::routes::path_segments;
using hivemind::routes::send_json;
using hivemind::routes::send_error;






} // namespace

void register_config_routes(OmniNet::HTTP::Server& server) {

    server.get("/api/config",
        [](const OmniNet::HTTP::Request&, OmniNet::HTTP::Response& resp) {
            auto& cfg = ServerConfig::instance();
            send_json(resp, json{
                {"workspace_root", cfg.workspace_root()},
                {"note", "workspace_root is bind-mounted at /workspace in "
                         "every agent container; changes apply to containers "
                         "created after the change"},
            });
        });

    server.put("/api/config",
        [](const OmniNet::HTTP::Request& req, OmniNet::HTTP::Response& resp) {
            json body;
            try { body = json::parse(req.bodyAsString()); }
            catch (...) { send_error(resp, 400, "invalid JSON"); return; }

            auto& cfg = ServerConfig::instance();
            try {
                if (body.contains("workspace_root")) {
                    if (!body["workspace_root"].is_string()) {
                        send_error(resp, 400, "workspace_root must be a string");
                        return;
                    }
                    cfg.set_workspace_root(
                        body["workspace_root"].get<std::string>());
                }
                send_json(resp, json{
                    {"workspace_root", cfg.workspace_root()},
                    {"applied", true},
                });
            } catch (const std::invalid_argument& e) {
                send_error(resp, 400, e.what());
            } catch (const std::exception& e) {
                send_error(resp, 500, e.what());
            }
        });
}
