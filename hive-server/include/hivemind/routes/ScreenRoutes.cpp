// include/hivemind/routes/ScreenRoutes.cpp — Phase 6 screen pipeline.
#include "hivemind/routes/ScreenRoutes.h"
#include "hivemind/routes/RouteUtil.h"
#include "hivemind/AgentStore.h"
#include "hivemind/AuthStore.h"
#include "hivemind/ContainerManager.h"

#include <nlohmann/json.hpp>

#include <chrono>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

using json = nlohmann::json;
using OmniNet::HTTP::Request;
using OmniNet::HTTP::Response;
namespace routes = hivemind::routes;

namespace {

constexpr const char* kScreenCookie = "hive_screen";

// ── Auth for /screen/* (browser-facing: no Authorization header) ────────
// Accepts: Bearer header, ?key= query, or the hive_screen cookie set on
// first ?key= access. Requires admin or agent scope — read-only keys must
// not watch screens (they can show sensitive sessions).
bool screen_authorized(const Request& req, AuthStore& auth) {
    std::string token;
    if (auto hdr = req.header("Authorization")) {
        if (hdr->substr(0, 7) == "Bearer ") token = hdr->substr(7);
    }
    if (token.empty())
        token = req.queryParameter("key", "");
    if (token.empty()) {
        if (auto cookie = req.header("Cookie")) {
            const std::string needle = std::string{kScreenCookie} + "=";
            auto pos = cookie->find(needle);
            if (pos != std::string::npos) {
                auto end = cookie->find(';', pos);
                token = cookie->substr(pos + needle.size(),
                                       end == std::string::npos
                                           ? end : end - pos - needle.size());
            }
        }
    }
    if (token.empty()) return false;
    auto scopes = auth.validate(token);
    if (!scopes) return false;
    return AuthStore::has_scope(*scopes, "admin") ||
           AuthStore::has_scope(*scopes, "agent");
}

// Resolve :id from /api/agents/:id/screen[/start|stop]; empty on mismatch.
std::string agent_id_from_api(const Request& req) {
    auto seg = routes::path_segments(req.path());   // api agents :id screen ..
    if (seg.size() < 4 || seg[0] != "api" || seg[1] != "agents" ||
        seg[3] != "screen") return {};
    return seg[2];
}

// ── WS splice: raw TCP relay after both handshakes ──────────────────────
void pump(OmniNet::Socket& from, OmniNet::Socket& to) {
    std::vector<uint8_t> buf(64 * 1024);
    for (;;) {
        size_t n = 0;
        try { n = from.recv(buf); } catch (...) { break; }
        if (n == 0) break;
        try { to.sendAll(std::span<const uint8_t>(buf.data(), n)); }
        catch (...) { break; }
    }
}

void splice_to_container(std::unique_ptr<OmniNet::Socket> client,
                         uint16_t port) {
    std::unique_ptr<OmniNet::Socket> up;
    try {
        up = OmniNet::createConnectedSocket("127.0.0.1", port,
                                            std::chrono::milliseconds(3000));
    } catch (...) { return; }   // client socket closes with `client`
    if (!up) return;

    // Fresh upstream handshake to websockify.
    const auto nonce = OmniNet::Crypto::randomBytes(16);
    const std::string key = OmniNet::Crypto::base64Encode(
        std::span<const uint8_t>(nonce.data(), nonce.size()));
    const std::string handshake =
        "GET /websockify HTTP/1.1\r\n"
        "Host: 127.0.0.1:" + std::to_string(port) + "\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Key: " + key + "\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        "Sec-WebSocket-Protocol: binary\r\n\r\n";
    try {
        up->sendAll(std::span<const uint8_t>(
            reinterpret_cast<const uint8_t*>(handshake.data()), handshake.size()));
        std::string status_line;
        for (int i = 0; i < 100; ++i) {          // read head, cap lines
            auto line = up->readLine();
            if (!line) return;
            if (status_line.empty()) status_line = *line;
            if (*line == "\r\n" || line->empty()) break;
        }
        if (status_line.find(" 101") == std::string::npos) return;
    } catch (...) { return; }

    // Bidirectional pump; when either direction dies, close both.
    std::thread t([&] {
        pump(*client, *up);
        try { up->shutdown(SHUT_WR); } catch (...) {}
    });
    pump(*up, *client);
    try { client->shutdown(SHUT_WR); } catch (...) {}
    t.join();
}

} // namespace

void register_screen_routes(OmniNet::HTTP::Server& server,
                            AgentStore& agents,
                            AuthStore& auth,
                            std::function<uint16_t(const std::string&)>
                                port_resolver) {
    // Resolve an agent's screen port: test override, else podman.
    // NOTE: capture BY VALUE — route handlers outlive this function.
    auto resolve = [port_resolver](const std::string& id) -> uint16_t {
        if (port_resolver) return port_resolver(id);
        return container_manager().screen_port(id);
    };
    // ── GET /api/agents/:id/screen — status ─────────────────────────────
    server.get("/api/agents/:id/screen",
        [&agents, resolve](const Request& req, Response& resp) {
            const std::string id = agent_id_from_api(req);
            if (id.empty() || !agents.get_agent(id)) {
                routes::send_error(resp, 404, "agent not found"); return;
            }
            auto& cm = container_manager();
            const uint16_t port = resolve(id);
            routes::send_json(resp, json{
                {"podman", ContainerManager::podman_available()},
                {"browser_image", cm.browser_image_present()},
                {"running", port != 0},
                {"port", port},
                {"url", port ? "/screen/" + id + "/" : ""},
            });
        });

    // ── POST /api/agents/:id/screen/start ───────────────────────────────
    server.post("/api/agents/:id/screen/start",
        [&agents](const Request& req, Response& resp) {
            const std::string id = agent_id_from_api(req);
            if (id.empty() || !agents.get_agent(id)) {
                routes::send_error(resp, 404, "agent not found"); return;
            }
            try {
                const uint16_t port = container_manager().ensure_screen(id);
                routes::send_json(resp, json{{"running", true},
                                             {"port", port},
                                             {"url", "/screen/" + id + "/"}});
            } catch (const std::exception& e) {
                routes::send_error(resp, 502, e.what());
            }
        });

    // ── POST /api/agents/:id/screen/stop ────────────────────────────────
    server.post("/api/agents/:id/screen/stop",
        [&agents](const Request& req, Response& resp) {
            const std::string id = agent_id_from_api(req);
            if (id.empty() || !agents.get_agent(id)) {
                routes::send_error(resp, 404, "agent not found"); return;
            }
            const bool stopped = container_manager().stop_screen(id);
            routes::send_json(resp, json{{"running", false}, {"stopped", stopped}});
        });

    // ── GET /screen/* — noVNC asset proxy (own auth) ────────────────────
    server.get("/screen/*",
        [&agents, &auth, resolve](const Request& req, Response& resp) {
            if (!screen_authorized(req, auth)) {
                routes::send_error(resp, 401, "missing or invalid key"); return;
            }
            auto seg = routes::path_segments(req.path());  // screen :id rest…
            if (seg.size() < 2 || !agents.get_agent(seg[1])) {
                routes::send_error(resp, 404, "agent not found"); return;
            }
            const uint16_t port = resolve(seg[1]);
            if (port == 0) {
                routes::send_error(resp, 503, "screen not running"); return;
            }

            // First ?key= access: set the cookie so relative asset loads
            // (and the WS handshake) authenticate without query params.
            // Deferred until resp is final — Response::ok() reassignment
            // wipes headers set beforehand (Phase 9 CORS bug class).
            std::string set_cookie;
            if (!req.queryParameter("key", "").empty())
                set_cookie = std::string{kScreenCookie} + "=" +
                             req.queryParameter("key", "") +
                             "; Path=/screen; HttpOnly; SameSite=Strict";

            // Root: redirect into noVNC with autoconnect. The key rides
            // along in the Location (clients without a cookie jar — e.g.
            // OmniNet's — follow without the cookie we just set).
            if (seg.size() == 2) {
                // Absolute Location — OmniNet's client won't follow relative
                // redirects; browsers accept either.
                std::string loc = "http://" +
                    std::string{req.header("Host").value_or("localhost")} +
                    "/screen/" + seg[1] +
                    "/vnc.html?autoconnect=true&resize=scale"
                    "&path=%2Fscreen%2Fws%3Fagent%3D" + seg[1] +   // ABSOLUTE — relative paths resolve against /screen/<id>/vnc.html and double up
                    "&password=" + container_manager().screen_password(seg[1]);
                if (!req.queryParameter("key", "").empty())
                    loc += "&key=" + req.queryParameter("key", "");
                if (req.queryParameter("view_only", "") == "1")
                    loc += "&view_only=true";   // Phase 28: watch, don't touch
                resp.setStatus(static_cast<OmniNet::HTTP::StatusCode>(302));
                resp.setHeader("Location", loc);
                if (!set_cookie.empty()) resp.setHeader("Set-Cookie", set_cookie);
                return;
            }

            // Asset proxy: /screen/<id>/<rest> → http://127.0.0.1:port/<rest>
            std::string rest;
            for (size_t i = 2; i < seg.size(); ++i) {
                if (seg[i] == "..") { routes::send_error(resp, 400, "bad path"); return; }
                rest += "/" + seg[i];
            }
            OmniNet::HTTP::ClientRequest creq;
            creq.method("GET")
                .url("http://127.0.0.1:" + std::to_string(port) + rest);
            auto res = OmniNet::HTTP::Client::get_instance().send_blocking(creq);
            if (!res) { routes::send_error(resp, 502, "upstream unreachable"); return; }

            resp = OmniNet::HTTP::Response::ok();
            resp.setStatus(res->status());
            static const char* kSkip[] = {"connection", "keep-alive",
                "transfer-encoding", "content-length", "upgrade"};
            for (auto& [k, v] : res->headers()) {
                std::string lk = k;
                for (auto& c : lk) c = static_cast<char>(std::tolower(c));
                bool skip = false;
                for (auto* s : kSkip) if (lk == s) { skip = true; break; }
                if (!skip) resp.setHeader(k, v);
            }
            resp.setBody(res->body());
            if (!set_cookie.empty()) resp.setHeader("Set-Cookie", set_cookie);
        });

    // ── WS /screen/ws?agent=<id> — splice to container websockify ────────
    OmniNet::HTTP::Server::ProtocolSwitchHandlers h;
    h.on_upgrade = [&agents, &auth, resolve](const Request& req, Response& resp) {
        const std::string id = req.queryParameter("agent", "");
        const uint16_t port = id.empty() ? 0 : resolve(id);
        if (!screen_authorized(req, auth) || id.empty() ||
            !agents.get_agent(id) || port == 0) {
            routes::send_error(resp, 403, "screen unavailable"); return;
        }
        auto wskey = req.header("Sec-WebSocket-Key");
        if (!wskey) { routes::send_error(resp, 400, "not a websocket request"); return; }
        resp.setStatus(static_cast<OmniNet::HTTP::StatusCode>(101));
        resp.setHeader("Upgrade", "websocket");
        resp.setHeader("Connection", "Upgrade");
        resp.setHeader("Sec-WebSocket-Accept",
                       OmniNet::WebSocket::Handshake::computeAcceptKey(std::string{*wskey}));
        if (auto proto = req.header("Sec-WebSocket-Protocol"))
            if (proto->find("binary") != std::string::npos)
                resp.setHeader("Sec-WebSocket-Protocol", "binary");
        resp.markProtocolSwitch();
    };
    h.on_post_upgrade = [resolve](std::unique_ptr<OmniNet::Socket> socket,
                           const Request& req) {
        const std::string id = req.queryParameter("agent", "");
        const uint16_t port = resolve(id);
        if (port == 0) return;
        std::thread(splice_to_container, std::move(socket), port).detach();
    };
    server.registerProtocolSwitch("/screen/ws", std::move(h));
}
