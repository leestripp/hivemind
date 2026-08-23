// [FILTERED_API_KEY]
// include/hivemind/routes/ModelRoutes.cpp
// -----------------------------------------------------------------------------
// /api/providers + /api/models — HiveMind-owned LLM configuration (Phase 13).
// LLMStore is the source of truth; credentials are injected into daedalus
// AgentConfig at agent-spawn time. /api/providers/:name/discover does a live
// GET /v1/models against the provider via daedalus' public discovery API.
// [FILTERED_API_KEY]

#include "hivemind/routes/ModelRoutes.h"

#include "hivemind/LLMStore.h"

#include <daedalus.h>
#include "daedalus/model_discovery.h"

#include <OmniNet.h>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

namespace {

void send_json(OmniNet::HTTP::Response& resp, const json& j, int status = 200) {
    auto r = status == 200   ? OmniNet::HTTP::Response::ok()
           : status == 201   ? OmniNet::HTTP::Response::created()
           : status == 404   ? OmniNet::HTTP::Response::notFound()
           : status == 400   ? OmniNet::HTTP::Response::badRequest()
           :                   OmniNet::HTTP::Response::ok();
    r.setContentType("application/json");
    r.setBody(j.dump());
    resp = r;
}

void send_err(OmniNet::HTTP::Response& resp, int status, const std::string& msg) {
    send_json(resp, json{{"error", msg}}, status);
}

json provider_json(const LlmProvider& p) {
    return {
        {"name",               p.name},
        {"host",               p.host},
        {"has_key",            !p.api_key.empty()},
        {"chat_endpoint",      p.chat_endpoint},
        {"discovery_endpoint", p.discovery_endpoint},
        {"timeout_seconds",    p.timeout_seconds},
        {"plugin",             p.plugin},
    };
}

json model_json(const LlmModel& m, bool has_key, const std::string& host,
                const std::string& chat_ep) {
    return {
        {"name",             m.name},
        {"provider",         m.provider},
        {"model",            m.model_id},
        {"host",             host},
        {"chat_endpoint",    chat_ep},
        {"max_context_size", m.max_context_size},
        {"temperature",      m.temperature},
        {"has_key",          has_key},
        {"is_default",       m.is_default},
    };
}

} // namespace

void register_model_routes(OmniNet::HTTP::Server& server, LLMStore& llm) {

    // ── GET /api/providers ────────────────────────────────────────────
    server.get("/api/providers",
        [&llm](const OmniNet::HTTP::Request&, OmniNet::HTTP::Response& resp) {
            json arr = json::array();
            for (auto& p : llm.list_providers()) arr.push_back(provider_json(p));
            send_json(resp, json{{"providers", arr}});
        });

    // ── POST /api/providers ───────────────────────────────────────────
    server.post("/api/providers",
        [&llm](const OmniNet::HTTP::Request& req, OmniNet::HTTP::Response& resp) {
            json b;
            try { b = json::parse(req.bodyAsString()); }
            catch (...) { send_err(resp, 400, "invalid JSON"); return; }
            LlmProvider p;
            p.name               = b.value("name", "");
            p.host               = b.value("host", "");
            p.api_key            = b.value("api_key", "");
            p.chat_endpoint      = b.value("chat_endpoint", "");
            p.discovery_endpoint = b.value("discovery_endpoint", "");
            p.timeout_seconds    = b.value("timeout_seconds", 30);
            p.plugin             = b.value("plugin", "");
            try {
                send_json(resp, provider_json(llm.create_provider(p)), 201);
            } catch (const std::invalid_argument& e) {
                send_err(resp, 400, e.what());
            }
        });

    // ── PATCH /api/providers/:name ────────────────────────────────────
    server.patch("/api/providers/:name",
        [&llm](const OmniNet::HTTP::Request& req, OmniNet::HTTP::Response& resp) {
            std::string name{req.pathParam(0)};
            auto existing = llm.get_provider(name);
            if (!existing) { send_err(resp, 404, "provider not found: " + name); return; }
            json b;
            try { b = json::parse(req.bodyAsString()); }
            catch (...) { send_err(resp, 400, "invalid JSON"); return; }
            LlmProvider p = *existing;
            if (b.contains("host"))               p.host = b["host"];
            if (b.contains("api_key"))            p.api_key = b["api_key"];
            if (b.contains("chat_endpoint"))      p.chat_endpoint = b["chat_endpoint"];
            if (b.contains("discovery_endpoint")) p.discovery_endpoint = b["discovery_endpoint"];
            if (b.contains("timeout_seconds"))    p.timeout_seconds = b["timeout_seconds"];
            if (b.contains("plugin"))             p.plugin = b["plugin"];
            if (!llm.update_provider(p)) { send_err(resp, 400, "update failed"); return; }
            send_json(resp, provider_json(*llm.get_provider(name)));
        });

    // ── DELETE /api/providers/:name ───────────────────────────────────
    server.delete_("/api/providers/:name",
        [&llm](const OmniNet::HTTP::Request& req, OmniNet::HTTP::Response& resp) {
            std::string name{req.pathParam(0)};
            if (!llm.delete_provider(name)) {
                send_err(resp, 400,
                    "cannot delete '" + name + "' — not found, or models still reference it");
                return;
            }
            send_json(resp, json{{"deleted", name}});
        });

    // ── GET /api/providers/:name/discover ─────────────────────────────
    // Live GET /v1/models against the provider (daedalus public API).
    server.get("/api/providers/:name/discover",
        [&llm](const OmniNet::HTTP::Request& req, OmniNet::HTTP::Response& resp) {
            std::string name{req.pathParam(0)};
            auto p = llm.get_provider(name);
            if (!p) { send_err(resp, 404, "provider not found: " + name); return; }
            auto res = daedalus::discover_models(p->host, p->api_key,
                                                 p->discovery_endpoint);
            if (!res.success) { send_err(resp, 502, res.error_message); return; }
            json arr = json::array();
            for (auto& m : res.models)
                arr.push_back({{"id", m.id}});
            send_json(resp, json{{"provider", name}, {"models", arr}});
        });

    // ── GET /api/models ───────────────────────────────────────────────
    // Response shape kept compatible with the pre-Phase-13 endpoint.
    server.get("/api/models",
        [&llm](const OmniNet::HTTP::Request&, OmniNet::HTTP::Response& resp) {
            json arr = json::array();
            for (auto& m : llm.list_models()) {
                auto r = llm.resolve(m.name);
                arr.push_back(model_json(m, r && !r->api_key.empty(),
                                         r ? r->host : "",
                                         r ? r->chat_endpoint : ""));
            }
            send_json(resp, arr);
        });

    // ── POST /api/models ──────────────────────────────────────────────
    server.post("/api/models",
        [&llm](const OmniNet::HTTP::Request& req, OmniNet::HTTP::Response& resp) {
            json b;
            try { b = json::parse(req.bodyAsString()); }
            catch (...) { send_err(resp, 400, "invalid JSON"); return; }
            LlmModel m;
            m.name             = b.value("name", "");
            m.provider         = b.value("provider", "");
            m.model_id         = b.value("model_id", "");
            m.max_context_size = b.value("max_context_size", 0);
            m.temperature      = b.value("temperature", 0.0);
            m.is_default       = b.value("is_default", false);
            try {
                auto created = llm.create_model(m);
                send_json(resp, model_json(created, false, "", ""), 201);
            } catch (const std::invalid_argument& e) {
                send_err(resp, 400, e.what());
            }
        });

    // ── GET /api/models/:name ─────────────────────────────────────────
    server.get("/api/models/:name",
        [&llm](const OmniNet::HTTP::Request& req, OmniNet::HTTP::Response& resp) {
            std::string name{req.pathParam(0)};
            auto m = llm.get_model(name);
            if (!m) { send_err(resp, 404, "model not found: " + name); return; }
            auto r = llm.resolve(name);
            send_json(resp, model_json(*m, r && !r->api_key.empty(),
                                       r ? r->host : "",
                                       r ? r->chat_endpoint : ""));
        });

    // ── PATCH /api/models/:name ───────────────────────────────────────
    server.patch("/api/models/:name",
        [&llm](const OmniNet::HTTP::Request& req, OmniNet::HTTP::Response& resp) {
            std::string name{req.pathParam(0)};
            auto existing = llm.get_model(name);
            if (!existing) { send_err(resp, 404, "model not found: " + name); return; }
            json b;
            try { b = json::parse(req.bodyAsString()); }
            catch (...) { send_err(resp, 400, "invalid JSON"); return; }
            LlmModel m = *existing;
            if (b.contains("provider"))         m.provider = b["provider"];
            if (b.contains("model_id"))         m.model_id = b["model_id"];
            if (b.contains("max_context_size")) m.max_context_size = b["max_context_size"];
            if (b.contains("temperature"))      m.temperature = b["temperature"];
            if (!llm.update_model(m)) { send_err(resp, 400, "update failed"); return; }
            send_json(resp, model_json(*llm.get_model(name), false, "", ""));
        });

    // ── POST /api/models/:name/default ────────────────────────────────
    server.post("/api/models/:name/default",
        [&llm](const OmniNet::HTTP::Request& req, OmniNet::HTTP::Response& resp) {
            std::string name{req.pathParam(0)};
            if (!llm.set_default_model(name)) {
                send_err(resp, 404, "model not found: " + name);
                return;
            }
            send_json(resp, json{{"default", name}});
        });

    // ── DELETE /api/models/:name ──────────────────────────────────────
    server.delete_("/api/models/:name",
        [&llm](const OmniNet::HTTP::Request& req, OmniNet::HTTP::Response& resp) {
            std::string name{req.pathParam(0)};
            if (!llm.delete_model(name)) {
                send_err(resp, 404, "model not found: " + name);
                return;
            }
            send_json(resp, json{{"deleted", name}});
        });
}
