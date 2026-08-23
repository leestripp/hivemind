// include/hivemind/routes/KeyRoutes.cpp — API key management (Phase 9).
#include "hivemind/routes/KeyRoutes.h"

#include <nlohmann/json.hpp>

#include <string>
#include <vector>

using json = nlohmann::json;

namespace {

std::vector<std::string> path_segments(std::string_view path) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : path) {
        if (c == '/') { if (!cur.empty()) { out.push_back(cur); cur.clear(); } }
        else cur += c;
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

void send_json(OmniNet::HTTP::Response& resp, const json& j, int status = 200) {
    resp = OmniNet::HTTP::Response::ok();
    resp.setStatus(static_cast<OmniNet::HTTP::StatusCode>(status));
    resp.setContentType("application/json");
    resp.setBody(j.dump());
}

void send_error(OmniNet::HTTP::Response& resp, int status, const std::string& msg) {
    send_json(resp, json{{"error", msg}}, status);
}

} // namespace

void register_key_routes(OmniNet::HTTP::Server& server, AuthStore& auth) {

    server.get("/api/keys",
        [&auth](const OmniNet::HTTP::Request&, OmniNet::HTTP::Response& resp) {
            json list = json::array();
            for (auto& k : auth.list()) {
                list.push_back({{"id", k.id}, {"name", k.name},
                                {"scopes", k.scopes},
                                {"created_at", k.created_at},
                                {"revoked", k.revoked}});
            }
            send_json(resp, json{{"keys", list}});
        });

    server.post("/api/keys",
        [&auth](const OmniNet::HTTP::Request& req, OmniNet::HTTP::Response& resp) {
            json body;
            try { body = json::parse(req.bodyAsString()); }
            catch (...) { send_error(resp, 400, "invalid JSON"); return; }

            std::string name = body.value("name", "");
            std::string scopes = body.value("scopes", "agent");
            if (name.empty()) { send_error(resp, 400, "name required"); return; }
            // Only known scopes; admin can only be minted by an admin key
            // (middleware already required admin to reach this route).
            for (auto& s : AuthStore::split_scopes(scopes)) {
                if (s != "admin" && s != "agent" && s != "read") {
                    send_error(resp, 400, "unknown scope: " + s); return;
                }
            }
            try {
                std::string raw = auth.create_key(name, scopes);
                send_json(resp, json{{"name", name}, {"scopes", scopes},
                                     {"key", raw},
                                     {"note", "store this now — it is never shown again"}}, 201);
            } catch (const std::exception& e) {
                send_error(resp, 500, e.what());
            }
        });

    server.delete_("/api/keys/:id",
        [&auth](const OmniNet::HTTP::Request& req, OmniNet::HTTP::Response& resp) {
            auto seg = path_segments(req.path());
            if (seg.size() < 3) { send_error(resp, 400, "missing id"); return; }
            if (!auth.revoke(seg[2])) {
                send_error(resp, 404, "key not found or already revoked"); return;
            }
            send_json(resp, json{{"revoked", seg[2]}});
        });
}
