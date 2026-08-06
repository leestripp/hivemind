#include "hivemind/Settings.h"
#include "hivemind/LLMSettings.h"
#include <OmniNet.h>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

void register_model_routes(OmniNet::HTTP::Server& server) {
    // GET /api/models  — returns model configs
    // Returns: [{name, model, host, chat_endpoint, max_context_size, has_key}, ...]
    server.get("/api/models",
        [](const OmniNet::HTTP::Request& req, OmniNet::HTTP::Response& resp) {
            (void)req;
            auto& settings = llm_settings();
            json arr = json::array();

            for (auto& [name, mc] : settings.models()) {
                arr.push_back({
                    {"name",             name},
                    {"model",            mc.model},
                    {"host",             mc.base_url},
                    {"chat_endpoint",    mc.chat_endpoint},
                    {"max_context_size", mc.max_context_size},
                    {"has_key",          !mc.api_key.empty()},
                });
            }

            OmniNet::HTTP::Response r = OmniNet::HTTP::Response::ok();
            r.setContentType("application/json");
            r.setBody(arr.dump());
            resp = r;
        });

    // GET /api/models/:name  — single model detail
    server.get("/api/models/:name",
        [](const OmniNet::HTTP::Request& req, OmniNet::HTTP::Response& resp) {
            std::string name{req.pathParam(0)};
            auto& settings = llm_settings();

            if (!settings.model_exists(name)) {
                auto r = OmniNet::HTTP::Response::notFound("model not found: " + name);
                r.setContentType("application/json");
                r.setBody(json{{"error", "model not found: " + name}}.dump());
                resp = r;
                return;
            }

            auto mc_opt = settings.get_model(name);
            if (!mc_opt) {
                auto r = OmniNet::HTTP::Response::notFound("model not found: " + name);
                r.setContentType("application/json");
                r.setBody(json{{"error", "model not found: " + name}}.dump());
                resp = r;
                return;
            }

            const auto& mc = *mc_opt;
            OmniNet::HTTP::Response r = OmniNet::HTTP::Response::ok();
            r.setContentType("application/json");
            r.setBody(json{
                {"name",             name},
                {"model",            mc.model},
                {"host",             mc.base_url},
                {"chat_endpoint",    mc.chat_endpoint},
                {"max_context_size", mc.max_context_size},
                {"has_key",          !mc.api_key.empty()},
            }.dump());
            resp = r;
        });
}
