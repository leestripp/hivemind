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
                {"screen_resolution", cfg.screen_resolution()},
                {"message_window", cfg.message_window()},
                {"note", "workspace_root is DEPRECATED/unused in v2: "
                         "workspaces are per-worker — each worker bind-mounts "
                         "~/.local/share/hive-worker/workspace at /workspace"},
            });
        });

    server.put("/api/config",
        [](const OmniNet::HTTP::Request& req, OmniNet::HTTP::Response& resp) {
            json body;
            try { body = json::parse(req.bodyAsString()); }
            catch (...) { send_error(resp, 400, "invalid JSON"); return; }

            auto& cfg = ServerConfig::instance();
            try {
                if (body.contains("screen_resolution")) {
                    if (!body["screen_resolution"].is_string()) {
                        send_error(resp, 400, "screen_resolution must be a string");
                        return;
                    }
                    cfg.set_screen_resolution(
                        body["screen_resolution"].get<std::string>());
                }
                if (body.contains("message_window")) {
                    if (!body["message_window"].is_number_integer()) {
                        send_error(resp, 400, "message_window must be an integer");
                        return;
                    }
                    cfg.set_message_window(body["message_window"].get<int>());
                }
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
                {"screen_resolution", cfg.screen_resolution()},
                    {"message_window", cfg.message_window()},
                    {"applied", true},
                });
            } catch (const std::invalid_argument& e) {
                send_error(resp, 400, e.what());
            } catch (const std::exception& e) {
                send_error(resp, 500, e.what());
            }
        });
}
