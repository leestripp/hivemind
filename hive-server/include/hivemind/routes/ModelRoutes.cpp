// include/hivemind/routes/ModelRoutes.cpp — Phase 43: hub-owned
// provider/model registry API. ONE place for LLM config; keys are
// stored here (llm.db) and injected into task frames at dispatch —
// never returned by any endpoint.
#include "hivemind/routes/ModelRoutes.h"
#include "hivemind/routes/RouteUtil.h"
#include "hivemind/LlmStore.h"
#include "hivemind/ModelCatalog.h"

#include <nlohmann/json.hpp>
#include <http/HttpClient.h>

#include <algorithm>
#include <cctype>

using json = nlohmann::json;
using hivemind::routes::path_segments;
using hivemind::routes::send_json;
using hivemind::routes::send_error;
using hivemind::routes::normalize_thinking_level;

namespace {

json provider_json(const ProviderInfo& p) {
    // Mask the key: presence + last 4 only. Raw keys never leave the hub.
    std::string masked;
    if (!p.api_key.empty())
        masked = "…" + p.api_key.substr(p.api_key.size() > 4 ? p.api_key.size() - 4 : 0);
    return json{{"name", p.name}, {"host", p.host},
                {"chat_endpoint", p.chat_endpoint}, {"plugin", p.plugin},
                {"api_key", masked}, {"has_key", !p.api_key.empty()},
                {"created_at", p.created_at}};
}

} // namespace

void register_model_routes(OmniNet::HTTP::Server& server, LlmStore& llm) {

    // ── Providers ───────────────────────────────────────────────────
    server.get("/api/providers",
        [&llm](const OmniNet::HTTP::Request&, OmniNet::HTTP::Response& resp) {
            json list = json::array();
            for (auto& p : llm.list_providers()) list.push_back(provider_json(p));
            send_json(resp, json{{"providers", list}});
        });

    server.post("/api/providers",
        [&llm](const OmniNet::HTTP::Request& req, OmniNet::HTTP::Response& resp) {
            json body = json::parse(req.body(), nullptr, false);
            if (body.is_discarded()) { send_error(resp, 400, "bad json"); return; }
            ProviderInfo p;
            p.name          = body.value("name", "");
            p.host          = body.value("host", "");
            p.chat_endpoint = body.value("chat_endpoint", "");
            p.plugin        = body.value("plugin", "");
            p.api_key       = body.value("api_key", "");
            if (p.name.empty()) { send_error(resp, 400, "name required"); return; }
            // Key omitted on update → keep the existing one.
            if (p.api_key.empty()) {
                auto existing = llm.get_provider(p.name);
                if (existing) p.api_key = existing->api_key;
                else { send_error(resp, 400, "api_key required for new provider"); return; }
            }
            try {
                llm.upsert_provider(p);
                send_json(resp, json{{"ok", true}, {"provider", provider_json(p)}});
            } catch (const std::exception& e) {
                send_error(resp, 400, e.what());
            }
        });

    server.delete_("/api/providers/:name",
        [&llm](const OmniNet::HTTP::Request& req, OmniNet::HTTP::Response& resp) {
            auto segs = path_segments(req.path());
            if (segs.size() < 3) { send_error(resp, 400, "bad path"); return; }
            if (!llm.remove_provider(segs[2])) {
                send_error(resp, 404, "unknown provider"); return;
            }
            send_json(resp, json{{"ok", true}, {"deleted", segs[2]}});
        });

    // Discover: GET {host}/v1/models with the provider's key (OpenAI
    // convention), auto-register every returned id as a model of this
    // provider. Raw key stays server-side; only names come back.
    server.post("/api/providers/:name/discover",
        [&llm](const OmniNet::HTTP::Request& req, OmniNet::HTTP::Response& resp) {
            auto segs = path_segments(req.path());
            if (segs.size() < 3) { send_error(resp, 400, "bad path"); return; }
            auto prov = llm.get_provider(segs[2]);
            if (!prov) { send_error(resp, 404, "unknown provider"); return; }
            if (prov->host.empty()) { send_error(resp, 400, "provider has no host"); return; }
            std::string base = prov->host;
            while (!base.empty() && base.back() == '/') base.pop_back();
            OmniNet::HTTP::ClientRequest rq;
            rq.method("GET").url(base + "/v1/models")
              .header("Authorization", "Bearer " + prov->api_key)
              .header("Accept", "application/json");
            auto r = OmniNet::HTTP::Client::get_instance().send_blocking(rq);
            if (!r) { send_error(resp, 502, "discover failed (transport error " + std::to_string((int)r.error()) + ")"); return; }
            if (r->statusCode() != 200) {
                send_error(resp, 502, "provider returned HTTP " +
                           std::to_string(r->statusCode()));
                return;
            }
            json doc = json::parse(r->body(), nullptr, false);
            if (doc.is_discarded() || !doc.contains("data") || !doc["data"].is_array()) {
                send_error(resp, 502, "unexpected /v1/models payload"); return;
            }
            const auto existing_models = llm.list_models();
            json added = json::array();
            for (const auto& m : doc["data"]) {
                const std::string id = m.value("id", "");
                if (id.empty()) continue;
                try {
                    LlmModelInfo mi;
                    mi.name = id; mi.provider = prov->name; mi.model_id = id;
                    const auto metadata = hivemind::model_metadata(prov->plugin, prov->host,
                                                         id, &m);
                    mi.max_context = metadata.max_context;
                    mi.temperature = metadata.temperature;
                    const auto existing = std::find_if(
                        existing_models.begin(), existing_models.end(),
                        [&id](const LlmModelInfo& old) { return old.name == id; });
                    if (existing != existing_models.end()) {
                        mi.thinking_level = existing->thinking_level;
                        if (mi.max_context == 0) mi.max_context = existing->max_context;
                        if (mi.temperature == 0.0) mi.temperature = existing->temperature;
                    }
                    llm.upsert_model(mi);
                    added.push_back(id);
                } catch (...) {}
            }
            send_json(resp, json{{"ok", true}, {"provider", prov->name},
                                 {"discovered", added}});
        });

    // ── Models ──────────────────────────────────────────────────────
    // Flat hub-side list: {models:[{name,provider}], default_model}.
    // The per-worker relay shape is gone — any model runs on any machine.
    server.get("/api/models",
        [&llm](const OmniNet::HTTP::Request&, OmniNet::HTTP::Response& resp) {
            json list = json::array();
            for (auto& m : llm.list_models())
                list.push_back({{"name", m.name}, {"provider", m.provider},
                                {"model_id", m.model_id},
                                {"max_context", m.max_context},
                                {"temperature", m.temperature},
                                {"thinking_level", m.thinking_level},
                                {"created_at", m.created_at}});
            send_json(resp, json{{"models", list},
                                 {"default_model", llm.default_model()}});
        });

    server.post("/api/models",
        [&llm](const OmniNet::HTTP::Request& req, OmniNet::HTTP::Response& resp) {
            json body = json::parse(req.body(), nullptr, false);
            if (body.is_discarded()) { send_error(resp, 400, "bad json"); return; }
            const std::string name = body.value("name", "");
            const std::string prov = body.value("provider", "");
            if (name.empty() || prov.empty()) {
                send_error(resp, 400, "name and provider required"); return;
            }
            try {
                LlmModelInfo mi;
                mi.name = name; mi.provider = prov;
                mi.model_id = body.value("model_id", "");
                mi.max_context = body.value("max_context", 0);
                mi.temperature = body.value("temperature", 0.0);
                if (body.contains("thinking_level")) {
                    if (!body["thinking_level"].is_string()) {
                        send_error(resp, 400,
                                   "thinking_level must be a string");
                        return;
                    }
                    mi.thinking_level = body["thinking_level"].get<std::string>();
                    if (!normalize_thinking_level(mi.thinking_level)) {
                        send_error(resp, 400,
                                   "thinking_level must be one of "
                                   "default|none|low|high|max");
                        return;
                    }
                    // models store no clear-sentinel: default == inherit
                    if (mi.thinking_level == "default")
                        mi.thinking_level.clear();
                }
                llm.upsert_model(mi);
                send_json(resp, json{{"ok", true}, {"model", name}});
            } catch (const std::exception& e) {
                send_error(resp, 400, e.what());
            }
        });

    server.delete_("/api/models/:name",
        [&llm](const OmniNet::HTTP::Request& req, OmniNet::HTTP::Response& resp) {
            auto segs = path_segments(req.path());
            if (segs.size() < 3) { send_error(resp, 400, "bad path"); return; }
            if (!llm.remove_model(segs[2])) {
                send_error(resp, 404, "unknown model"); return;
            }
            send_json(resp, json{{"ok", true}, {"deleted", segs[2]}});
        });

    server.post("/api/models/default",
        [&llm](const OmniNet::HTTP::Request& req, OmniNet::HTTP::Response& resp) {
            json body = json::parse(req.body(), nullptr, false);
            if (body.is_discarded()) { send_error(resp, 400, "bad json"); return; }
            try {
                llm.set_default_model(body.value("name", ""));
                send_json(resp, json{{"ok", true},
                                     {"default_model", llm.default_model()}});
            } catch (const std::exception& e) {
                send_error(resp, 400, e.what());
            }
        });
}
