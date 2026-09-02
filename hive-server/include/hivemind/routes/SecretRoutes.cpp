// include/hivemind/routes/SecretRoutes.cpp
#include "hivemind/routes/SecretRoutes.h"
#include "hivemind/Vault.h"
#include "hivemind/routes/RouteUtil.h"
#include "hivemind/AgentStore.h"
#include "hivemind/SecretStore.h"

#include <nlohmann/json.hpp>

using json = nlohmann::json;
using hivemind::routes::path_segments;
using hivemind::routes::send_json;
using hivemind::routes::send_error;

void register_secret_routes(OmniNet::HTTP::Server& server,
                            AgentStore& agents,
                            hivemind::SecretStore& secrets) {

    // ── GET /api/secrets — names + metadata + grants; VALUES NEVER LEAVE ──
    server.get("/api/secrets",
        [&agents, &secrets](const OmniNet::HTTP::Request&, OmniNet::HTTP::Response& resp) {
            json list = json::array();
            for (auto& s : secrets.list()) {
                json granted = json::array();
                for (auto& aid : s.agent_ids)
                    if (auto a = agents.get_agent(aid))
                        granted.push_back({{"id", a->id}, {"name", a->name}});
                list.push_back({{"name", s.name},
                                {"created_at", s.created_at},
                                {"type", s.type},
                                {"username", s.username},  // login only
                                {"url", s.url},            // login only
                                {"source", s.source},   // Phase 22: vault ref
                                {"agents", granted}});
            }
            send_json(resp, json{{"secrets", list}});
        });

    // ── POST /api/secrets — create (write-only value) ─────────────────────
    server.post("/api/secrets",
        [&secrets](const OmniNet::HTTP::Request& req, OmniNet::HTTP::Response& resp) {
            json body;
            try { body = json::parse(req.bodyAsString()); }
            catch (...) { send_error(resp, 400, "invalid JSON"); return; }
            const std::string name = body.value("name", "");
            const std::string value = body.value("value", "");
            const std::string source = body.value("source", "");
            const std::string type = body.value("type", "api_key");
            if (!hivemind::SecretStore::valid_name(name)) {
                send_error(resp, 400,
                    "name must be [A-Za-z0-9_], start with a letter/underscore, ≤64 chars");
                return;
            }
            if (!value.empty() && !source.empty()) {
                send_error(resp, 400, "give 'value' OR 'source', not both");
                return;
            }
            if (!source.empty()) {
                // Phase 22: external vault reference — value fetched at
                // delivery time, never stored here.
                const std::string bad = hm::vault::validate_source(source);
                if (!bad.empty()) { send_error(resp, 400, bad); return; }
                if (!secrets.put_external(name, source)) {
                    send_error(resp, 409, "secret exists (delete first to rotate)");
                    return;
                }
                send_json(resp, json{{"created", name}, {"source", source}}, 201);
                return;
            }
            if (!hivemind::SecretStore::valid_type(type)) {
                send_error(resp, 400, "type must be api_key | login | note");
                return;
            }
            json payload;
            if (type == "login") {
                payload = {{"username", body.value("username", "")},
                           {"password", body.value("password", "")},
                           {"url",      body.value("url", "")}};
                if (payload["password"].get<std::string>().empty()) {
                    send_error(resp, 400, "login requires a password");
                    return;
                }
            } else if (type == "note") {
                payload = {{"text", body.value("text", "")}};
                if (payload["text"].get<std::string>().empty()) {
                    send_error(resp, 400, "note requires text");
                    return;
                }
            } else {
                if (value.find('\n') != std::string::npos ||
                    value.find('\r') != std::string::npos) {
                    send_error(resp, 400, "multiline api keys not supported"
                                          " (use type=note)");
                    return;
                }
                payload = {{"value", value}};
                if (value.empty()) {
                    send_error(resp, 400, "value required");
                    return;
                }
            }
            if (!secrets.put_payload(name, type, payload)) {
                send_error(resp, 409, "secret exists (delete first to rotate)"
                                      " or payload invalid");
                return;
            }
            send_json(resp, json{{"created", name}, {"type", type}}, 201);
        });

    // ── DELETE /api/secrets/:name — remove + cascade grants ───────────────
    server.delete_("/api/secrets/:name",
        [&secrets](const OmniNet::HTTP::Request& req, OmniNet::HTTP::Response& resp) {
            auto seg = path_segments(req.path());   // api secrets :name
            if (seg.size() < 3) { send_error(resp, 400, "bad path"); return; }
            if (!secrets.remove(seg[2])) { send_error(resp, 404, "not found"); return; }
            send_json(resp, json{{"deleted", seg[2]}});
        });

    // ── GET /api/agents/:id/secrets — granted names ───────────────────────
    server.get("/api/agents/:id/secrets",
        [&agents, &secrets](const OmniNet::HTTP::Request& req, OmniNet::HTTP::Response& resp) {
            auto seg = path_segments(req.path());   // api agents :id secrets
            if (seg.size() < 4) { send_error(resp, 400, "bad path"); return; }
            if (!agents.get_agent(seg[2])) { send_error(resp, 404, "agent not found"); return; }
            send_json(resp, json{{"secrets", secrets.grants_for_agent(seg[2])}});
        });

    // ── POST /api/agents/:id/secrets — {name, granted} toggle ─────────────
    server.post("/api/agents/:id/secrets",
        [&agents, &secrets](const OmniNet::HTTP::Request& req, OmniNet::HTTP::Response& resp) {
            auto seg = path_segments(req.path());
            if (seg.size() < 4) { send_error(resp, 400, "bad path"); return; }
            if (!agents.get_agent(seg[2])) { send_error(resp, 404, "agent not found"); return; }
            json body;
            try { body = json::parse(req.bodyAsString()); }
            catch (...) { send_error(resp, 400, "invalid JSON"); return; }
            const std::string name = body.value("name", "");
            const bool granted = body.value("granted", true);
            if (!secrets.exists(name)) { send_error(resp, 404, "secret not found"); return; }
            const bool ok = granted ? secrets.grant(name, seg[2])
                                    : secrets.revoke(name, seg[2]);
            if (granted && !ok) { send_error(resp, 500, "grant failed"); return; }
            send_json(resp, json{{"secret", name}, {"granted", granted}});
        });
}
