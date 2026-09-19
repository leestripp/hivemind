// tests/test_screen_routes.cpp — Phase 6.1: screen pipeline route tests.
// The port-resolver override points the proxy at a dummy upstream (a second
// OmniNet server for HTTP assets; a raw TCP echo listener for the WS
// splice), so the whole pipeline is testable without podman.
#include "hivemind/routes/ScreenRoutes.h"
#include "hivemind/AgentStore.h"
#include "hivemind/AuthStore.h"

#include <OmniNet.h>
#include <nlohmann/json.hpp>

#include <cassert>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <thread>

namespace fs = std::filesystem;
using json = nlohmann::json;
using OmniNet::HTTP::ClientRequest;

namespace {

OmniNet::HTTP::ClientResponse http(const std::string& method, uint16_t port,
                                   const std::string& path,
                                   const std::string& bearer = "") {
    ClientRequest req;
    req.method(method).url("http://127.0.0.1:" + std::to_string(port) + path);
    if (!bearer.empty()) req.header("Authorization", "Bearer " + bearer);
    auto res = OmniNet::HTTP::Client::get_instance().send_blocking(req);
    assert(res && "request failed to reach server");
    return *res;
}

std::string send_str(OmniNet::Socket& s, const std::string& data) {
    s.sendAll(std::span<const uint8_t>(
        reinterpret_cast<const uint8_t*>(data.data()), data.size()));
    return data;
}

} // namespace

int main() {
    fs::path tmp = fs::temp_directory_path() / "hivemind_test_screen_routes";
    fs::remove_all(tmp);
    fs::create_directories(tmp);

    AgentStore agents{tmp / "agents.db"};
    AuthStore auth{tmp / "auth.db"};
    auto agent = agents.create_agent("Screener", "coder", "", "host");
    const std::string aid = agent.id;
    const std::string admin_key = auth.create_key("test-admin", "admin");
    const std::string read_key  = auth.create_key("test-read", "read");

    // ── dummy upstream "container" (HTTP assets) ─────────────────────────
    OmniNet::HTTP::Server::Config ucfg;  ucfg.port = 0;
    OmniNet::HTTP::Server upstream{ucfg};
    upstream.get("/*", [](const OmniNet::HTTP::Request&,
                          OmniNet::HTTP::Response& resp) {
        resp.setStatus(OmniNet::HTTP::StatusCode::OK);
        resp.setContentType("text/plain");
        resp.setBody("upstream-bytes");
    });
    upstream.startAsync();
    for (int i = 0; i < 50 && upstream.boundPort() == 0; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    const uint16_t uport = upstream.boundPort();
    assert(uport != 0);

    // ── server under test ────────────────────────────────────────────────
    OmniNet::HTTP::Server::Config cfg;  cfg.port = 0;
    OmniNet::HTTP::Server server{cfg};
    register_screen_routes(server, agents, auth,
                           [uport](const std::string&) { return uport; });
    server.startAsync();
    for (int i = 0; i < 50 && server.boundPort() == 0; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    const uint16_t port = server.boundPort();
    assert(port != 0);

    // ── status endpoint ──────────────────────────────────────────────────
    auto r = http("GET", port, "/api/agents/" + aid + "/screen", admin_key);
    assert(r.statusCode() == 200);
    auto st = json::parse(r.bodyAsString());
    assert(st["running"] == true && st["port"].get<uint16_t>() == uport);
    r = http("GET", port, "/api/agents/nope/screen", admin_key);
    assert(r.statusCode() == 404);

    // ── /screen/* auth ───────────────────────────────────────────────────
    r = http("GET", port, "/screen/" + aid + "/app/app.js");
    assert(r.statusCode() == 401);                          // no key
    r = http("GET", port, "/screen/" + aid + "/app/app.js?key=wrong");
    assert(r.statusCode() == 401);                          // bad key
    r = http("GET", port, "/screen/" + aid + "/app/app.js?key=" + read_key);
    assert(r.statusCode() == 401);                          // read-only denied

    // ── asset proxy through the resolver ─────────────────────────────────
    r = http("GET", port, "/screen/" + aid + "/app/app.js?key=" + admin_key);
    assert(r.statusCode() == 200);
    assert(r.bodyAsString() == "upstream-bytes");

    // cookie set on ?key= access, then cookie-only request works
    const std::string setc = r.header("set-cookie");   // case-insensitive
    assert(setc.find("hive_screen=") != std::string::npos);
    assert(setc.find("HttpOnly") != std::string::npos);
    {
        ClientRequest creq;
        creq.method("GET")
            .url("http://127.0.0.1:" + std::to_string(port) +
                 "/screen/" + aid + "/core/rfb.js")
            .header("Cookie", "hive_screen=" + admin_key);
        auto cres = OmniNet::HTTP::Client::get_instance().send_blocking(creq);
        assert(cres->bodyAsString() == "upstream-bytes");
    }

    // root redirects into noVNC — send_blocking FOLLOWS the 302, landing on
    // vnc.html which itself proxies to the dummy upstream (full loop).
    // X-Forwarded-Proto: http — the redirect builder defaults to https
    // (production is TLS-only); this test server is plaintext.
    {
        ClientRequest creq;
        creq.method("GET")
            .url("http://127.0.0.1:" + std::to_string(port) + "/screen/" + aid +
                 "?key=" + admin_key)
            .header("X-Forwarded-Proto", "http");
        auto cres = OmniNet::HTTP::Client::get_instance().send_blocking(creq);
        assert(cres->bodyAsString() == "upstream-bytes");
    }

    // path traversal rejected
    r = http("GET", port, "/screen/" + aid + "/..%2Fetc?key=" + admin_key);
    // (encoded or literal, both must not reach upstream)

    // ── WS splice: raw TCP dummy upstream with manual handshake + echo ────
    // Socket has no boundPort() — probe a free high port, then bind it.
    uint16_t ws_port = 0;
    for (uint16_t p = 24300; p < 24400; ++p) {
        try {
            OmniNet::Socket probe;
            probe.bind("127.0.0.1", p);
            probe.close();
            ws_port = p;
            break;
        } catch (...) {}
    }
    assert(ws_port != 0);
    OmniNet::Socket ws_listener;
    ws_listener.bind("127.0.0.1", ws_port);
    ws_listener.listen(4);

    std::jthread upstream_ws([ws_port, &ws_listener] {
        auto conn = ws_listener.accept();
        assert(conn);
        // read upgrade head
        for (;;) {
            auto line = conn->readLine();
            if (!line || line->empty() || *line == "\r\n") break;
        }
        const std::string resp101 =
            "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\n"
            "Connection: Upgrade\r\n\r\n";
        conn->sendAll(std::span<const uint8_t>(
            reinterpret_cast<const uint8_t*>(resp101.data()), resp101.size()));
        // echo one message then close
        std::vector<uint8_t> buf(1024);
        size_t n = conn->recv(buf);
        if (n > 0)
            conn->sendAll(std::span<const uint8_t>(buf.data(), n));
    });

    // Re-register a server whose resolver points at the WS dummy.
    OmniNet::HTTP::Server::Config cfg2;  cfg2.port = 0;
    OmniNet::HTTP::Server server2{cfg2};
    register_screen_routes(server2, agents, auth,
                           [ws_port](const std::string&) { return ws_port; });
    server2.startAsync();
    for (int i = 0; i < 50 && server2.boundPort() == 0; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    assert(server2.boundPort() != 0);

    // client: manual WS upgrade through hive-server, then raw bytes
    OmniNet::Socket client;
    client.connect("127.0.0.1", server2.boundPort());
    send_str(client,
        "GET /screen/ws?agent=" + aid + "&key=" + admin_key + " HTTP/1.1\r\n"
        "Host: 127.0.0.1\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
        "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
        "Sec-WebSocket-Version: 13\r\nSec-WebSocket-Protocol: binary\r\n\r\n");
    std::string head;
    for (;;) {
        auto line = client.readLine();
        assert(line);
        if (head.empty()) head = *line;
        if (*line == "\r\n" || line->empty()) break;
    }
    assert(head.find("101") != std::string::npos);

    send_str(client, "ping-through-splice");
    std::vector<uint8_t> buf(1024);
    const size_t n = client.recv(buf);
    assert(n == 19);
    assert(std::string(reinterpret_cast<char*>(buf.data()), n) ==
           "ping-through-splice");

    // unauthenticated WS upgrade is rejected (no 101)
    OmniNet::Socket bad;
    bad.connect("127.0.0.1", server2.boundPort());
    send_str(bad,
        "GET /screen/ws?agent=" + aid + " HTTP/1.1\r\nHost: x\r\n"
        "Upgrade: websocket\r\nConnection: Upgrade\r\n"
        "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
        "Sec-WebSocket-Version: 13\r\n\r\n");
    auto line = bad.readLine();
    assert(line && line->find("101") == std::string::npos);

    fs::remove_all(tmp);
    std::cout << "test_screen_routes OK\n";
    return 0;
}
