// [FILTERED_API_KEY]
// include/hivemind/routes/ModelRoutes.cpp
// -----------------------------------------------------------------------------
// /api/models — lists models configured in daedalus' own SettingsStore
// (SQLite at ~/.local/share/daedalus/daedalus.db). HiveMind keeps no model
// settings of its own; daedalus is the single source of truth.
// [FILTERED_API_KEY]

#include "hivemind/routes/ModelRoutes.h"

#include <daedalus.h>
#include "daedalus/settings.h"

#include <OmniNet.h>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

void register_model_routes(OmniNet::HTTP::Server& server) {
    // GET /api/models — [{name, provider, model_id, host, chat_endpoint,
    //                     max_context_size, has_key, is_default}, ...]
    server.get("/api/models",
        [](const OmniNet::HTTP::Request& req, OmniNet::HTTP::Response& resp) {
            (void)req;
            auto& store = daedalus::SettingsStore::instance();
            const std::string def = store.get_default_model_name();

            json arr = json::array();
            for (auto& m : store.list_models()) {
                auto resolved = store.resolve_model(m.name);
                arr.push_back({
                    {"name",             m.name},
                    {"provider",         m.provider},
                    {"model",            m.model_id},
                    {"host",             resolved ? resolved->host        : ""},
                    {"chat_endpoint",    resolved ? resolved->chat_endpoint : ""},
                    {"max_context_size", m.max_context_size},
                    {"has_key",          resolved ? !resolved->api_key.empty() : false},
                    {"is_default",       m.name == def},
                });
            }

            OmniNet::HTTP::Response r = OmniNet::HTTP::Response::ok();
            r.setContentType("application/json");
            r.setBody(arr.dump());
            resp = r;
        });

    // GET /api/models/:name — single model detail
    server.get("/api/models/:name",
        [](const OmniNet::HTTP::Request& req, OmniNet::HTTP::Response& resp) {
            std::string name{req.pathParam(0)};
            auto& store = daedalus::SettingsStore::instance();

            auto m = store.get_model(name);
            if (!m) {
                OmniNet::HTTP::Response r = OmniNet::HTTP::Response::notFound();
                r.setContentType("application/json");
                r.setBody(json{{"error", "model not found: " + name}}.dump());
                resp = r;
                return;
            }

            auto resolved = store.resolve_model(name);
            OmniNet::HTTP::Response r = OmniNet::HTTP::Response::ok();
            r.setContentType("application/json");
            r.setBody(json{
                {"name",             m->name},
                {"provider",         m->provider},
                {"model",            m->model_id},
                {"host",             resolved ? resolved->host          : ""},
                {"chat_endpoint",    resolved ? resolved->chat_endpoint : ""},
                {"max_context_size", m->max_context_size},
                {"has_key",          resolved ? !resolved->api_key.empty() : false},
                {"is_default",       name == store.get_default_model_name()},
            }.dump());
            resp = r;
        });
}
