// include/hivemind/routes/ScreenRoutes.cpp — Phase 6 screen pipeline.
#include "hivemind/routes/ScreenRoutes.h"
#include "hivemind/routes/RouteUtil.h"
#include "hivemind/AgentStore.h"
#include "hivemind/AuthStore.h"
#include "hivemind/WorkerRouter.h"

#include <nlohmann/json.hpp>

#include <chrono>
#include <map>
#include <mutex>
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

// 30.4: the upstream is any connected Socket — a direct TCP socket for
// local containers, a ChannelSocket over the worker channel for remote.
void splice_to_container(std::unique_ptr<OmniNet::Socket> client,
                         std::unique_ptr<OmniNet::Socket> up,
                         uint16_t port) {

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

std::unique_ptr<OmniNet::Socket> connect_upstream(const std::string& id,
                                                  uint16_t port) {
    // Remote agent? ChannelSocket over the worker channel (throws on
    // offline/open-failure — caller treats as "screen unavailable").
    if (auto remote = WorkerRouter::screen_stream(id, port)) return remote;
    try {
        return OmniNet::createConnectedSocket("127.0.0.1", port,
                                              std::chrono::milliseconds(3000));
    } catch (...) { return nullptr; }
}

/// HTTP GET over an arbitrary Socket (used for REMOTE noVNC assets —
/// the local path uses OmniNet's HTTP client). Connection: close gives
/// EOF-terminated bodies; returns {status, headers-skip-listed, body}.
bool http_fetch(OmniNet::Socket& sock, uint16_t port,
                const std::string& path, Response& resp) {
    const std::string req =
        "GET " + path + " HTTP/1.1\r\nHost: 127.0.0.1:" +
        std::to_string(port) + "\r\nConnection: close\r\n\r\n";
    try {
        sock.sendAll(std::span<const uint8_t>(
            reinterpret_cast<const uint8_t*>(req.data()), req.size()));
        std::string status_line;
        for (int i = 0; i < 100; ++i) {
            auto line = sock.readLine();
            if (!line) return false;
            if (status_line.empty()) {
                status_line = *line;   // "HTTP/1.1 200 OK"
                continue;
            }
            if (line->empty() || *line == "\r") break;
            auto colon = line->find(':');
            if (colon == std::string::npos) continue;
            std::string k = line->substr(0, colon);
            std::string v = line->substr(colon + 1);
            while (!v.empty() && v.front() == ' ') v.erase(v.begin());
            std::string lk = k;
            for (auto& c : lk) c = static_cast<char>(std::tolower(c));
            static const char* kSkip[] = {"connection", "keep-alive",
                "transfer-encoding", "content-length", "upgrade"};
            bool skip = false;
            for (auto* sk : kSkip) if (lk == sk) { skip = true; break; }
            if (!skip) resp.setHeader(k, v);
        }
        int status = 200;
        if (sscanf(status_line.c_str(), "HTTP/%*s %d", &status) != 1)
            status = 502;
        resp.setStatus(static_cast<OmniNet::HTTP::StatusCode>(status));
        std::string body;
        for (;;) {
            auto chunk = sock.recvAll(16384);
            if (chunk.empty()) break;
            body.append(reinterpret_cast<const char*>(chunk.data()),
                        chunk.size());
        }
        resp.setBody(body);
        return true;
    } catch (...) { return false; }
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
        // 30.4: worker-assigned agent → screen_ctl status over channel.
        try {
            if (auto r = WorkerRouter::screen_ctl(id, "status"))
                return (r->error.empty() && r->running) ? r->port
                                                        : uint16_t{0};
        } catch (...) { return 0; }   // worker offline → screen down
        return 0;   // v2: no worker -> no screen
    };
    // 30.4: remote screen password (rides in the redirect URL).
    auto password_of = [](const std::string& id) -> std::string {
        try {
            if (auto r = WorkerRouter::screen_ctl(id, "status"))
                return r->password;
        } catch (...) {}
        return {};   // v2: no worker -> no password
    };
    // ── GET /api/agents/:id/screen — status ─────────────────────────────
    server.get("/api/agents/:id/screen",
        [&agents, resolve](const Request& req, Response& resp) {
            const std::string id = agent_id_from_api(req);
            if (id.empty() || !agents.get_agent(id)) {
                routes::send_error(resp, 404, "agent not found"); return;
            }
            const uint16_t port = resolve(id);
            // v2: podman/image facts live on the WORKER - the channel is
            // the only source of truth; no worker -> screen stack down.
            // screen_ctl THROWS when the worker is offline — report 503
            // (start handler does), not an uncaught 500.
            std::optional<WorkerChannel::ScreenResult> remote;
            try { remote = WorkerRouter::screen_ctl(id, "status"); }
            catch (const std::exception& e) {
                routes::send_error(resp, 503, e.what()); return;
            }
            routes::send_json(resp, json{
                {"podman", remote.has_value()},
                {"browser_image", remote.has_value() && remote->error.empty()},
                {"running", port != 0},
                {"port", port},
                {"url", port ? "/screen/" + id + "/" : ""},
            });
        });

    // ── POST /api/agents/:id/screen/start ───────────────────────────────
    server.post("/api/agents/:id/screen/start",
        [&agents](const Request& req, Response& resp) {
            const std::string id = agent_id_from_api(req);
            const auto agent_rec = id.empty() ? std::nullopt
                                              : agents.get_agent(id);
            if (id.empty() || !agent_rec) {
                routes::send_error(resp, 404, "agent not found"); return;
            }
            // Phase 72.2: host agents DO have a screen — each hivex
            // user gets its own Xvfb slot on the worker host (display
            // :99+slot, websockify 6080+slot); the worker's
            // screen_ctl_host drives it. No hub-side gate.
            (void)agent_rec;
            try {
                // 30.4: worker-assigned → screen_ctl start over channel.
                if (auto r = WorkerRouter::screen_ctl(id, "start")) {
                    if (!r->error.empty()) {
                        routes::send_error(resp, 502, r->error); return;
                    }
                    routes::send_json(resp, json{{"running", r->running},
                                                 {"port", r->port},
                                                 {"url", "/screen/" + id + "/"}});
                    return;
                }
                routes::send_error(resp, 503, "agent's worker is offline");
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
            // 30.4: worker-assigned → screen_ctl stop over channel.
            try {
                if (WorkerRouter::screen_ctl(id, "stop")) {
                    routes::send_json(resp, json{{"running", false},
                                                 {"stopped", true}});
                    return;
                }
            } catch (const std::exception& e) {
                routes::send_error(resp, 502, e.what()); return;
            }
            routes::send_error(resp, 503, "agent's worker is offline");
        });

    // ── GET /screen/* — noVNC asset proxy (own auth) ────────────────────
    server.get("/screen/*",
        [&agents, &auth, resolve, password_of](const Request& req, Response& resp) {
            if (!screen_authorized(req, auth)) {
                routes::send_error(resp, 401, "missing or invalid key"); return;
            }
            auto seg = routes::path_segments(req.path());  // screen :id rest…
            if (seg.size() < 2 || !agents.get_agent(seg[1])) {
                routes::send_error(resp, 404, "agent not found"); return;
            }
            const uint16_t port = resolve(seg[1]);
            if (port == 0) {
                // Phase 66f: SELF-HEALING share link. A parked/stopped
                // screen used to dead-end here with a bare 503 — the
                // recurring "forgot noVNC again" report. Instead: kick
                // an async start on the worker (pin + maybe recreate +
                // Xvfb/x11vnc/websockify + pasta grace = 10-45s) and
                // serve an auto-refresh page; the next load lands on
                // live noVNC. A 60s in-flight window dedups refreshes
                // so concurrent starts can't stack (double websockify).
                const std::string id = seg[1];
                static std::mutex starts_m;
                static std::map<std::string, std::chrono::steady_clock::time_point>
                    starts;
                bool kick = false;
                {
                    std::lock_guard<std::mutex> lk(starts_m);
                    auto it = starts.find(id);
                    const auto now = std::chrono::steady_clock::now();
                    if (it == starts.end() ||
                        now - it->second > std::chrono::seconds(60)) {
                        starts[id] = now;
                        kick = true;
                    }
                }
                if (kick)
                    std::thread([id] {
                        try { WorkerRouter::screen_ctl(id, "start"); }
                        catch (...) {}
                    }).detach();
                resp = OmniNet::HTTP::Response::ok();
                resp.setContentType("text/html; charset=utf-8");
                resp.setHeader("Cache-Control",
                               "no-cache, no-store, must-revalidate");
                resp.setHeader("Refresh", "8");
                resp.setBody(
                    "<!doctype html><meta charset=utf-8>"
                    "<title>Starting screen\u2026</title>"
                    "<body style='font-family:sans-serif;background:#111;"
                    "color:#eee;display:flex;align-items:center;"
                    "justify-content:center;height:100vh;margin:0'>"
                    "<div style='text-align:center'>"
                    "<h2>Screen session is starting\u2026</h2>"
                    "<p>The desktop stack was parked. It is being woken "
                    "now \u2014 this page refreshes automatically "
                    "(usually 10\u201340 seconds).</p></div>");
                return;
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
                // redirects; browsers accept either. Scheme: behind TLS
                // nginx the hardcoded http:// downgraded the noVNC page to
                // plaintext (and its WS to ws://) — honour X-Forwarded-Proto,
                // default https (the site is TLS-only).
                std::string scheme = "https";
                if (auto xfp = req.header("X-Forwarded-Proto"))
                    if (!xfp->empty()) scheme = *xfp;
                std::string loc = scheme + "://" +
                    std::string{req.header("Host").value_or("localhost")} +
                    "/screen/" + seg[1] +
                    "/vnc.html?autoconnect=true&resize=scale"
                    "&path=%2Fscreen%2Fws%3Fagent%3D" + seg[1] +   // ABSOLUTE — relative paths resolve against /screen/<id>/vnc.html and double up
                    "&password=" + password_of(seg[1]);
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

            // 30.4: remote agent → fetch over the worker channel.
            {
                std::unique_ptr<OmniNet::Socket> remote;
                try { remote = WorkerRouter::screen_stream(seg[1], port); }
                catch (const std::exception& e) {
                    routes::send_error(resp, 503, e.what()); return;
                }
                if (remote) {
                    resp = OmniNet::HTTP::Response::ok();
                    if (!http_fetch(*remote, port, rest, resp)) {
                        routes::send_error(resp, 502,
                                           "remote screen asset fetch failed");
                        return;
                    }
                    if (!set_cookie.empty())
                        resp.setHeader("Set-Cookie", set_cookie);
                    return;
                }
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
        std::thread([client = std::move(socket), id, port]() mutable {
            std::unique_ptr<OmniNet::Socket> up;
            try { up = connect_upstream(id, port); }
            catch (...) { return; }   // client socket closes with `client`
            if (!up) return;
            splice_to_container(std::move(client), std::move(up), port);
        }).detach();
    };
    server.registerProtocolSwitch("/screen/ws", std::move(h));
}
