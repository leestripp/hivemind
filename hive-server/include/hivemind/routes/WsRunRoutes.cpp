// =============================================================================
// include/hivemind/routes/WsRunRoutes.cpp
// -----------------------------------------------------------------------------
// WebSocket /ws/runs — live streaming of run events.
//
// Implemented with OmniNet's PUBLIC protocol-switch API
// (HTTP::Server::registerProtocolSwitch) — no OmniNet patches. The
// handshake mirrors WebSocket::Server::on() (docs/api/websocket.md).
// =============================================================================

#include "hivemind/routes/WsRunRoutes.h"
#include "hivemind/routes/RunRoutes.h"

#include <OmniNet.h>
#include "websocket/WebSocket.h"
#include "websocket/WebSocketHandshake.h"
#include "utils/Utils.h"

#include <nlohmann/json.hpp>

#include <memory>
#include <mutex>
#include <iostream>
#include <sstream>
#include <thread>
#include <unordered_map>
#include <vector>

using json = nlohmann::json;

namespace {

struct WsSubscriber {
    std::shared_ptr<OmniNet::WebSocket::Connection> conn;
    std::mutex send_mtx;   // guard sendText — events come from runner threads
};

// run_id → subscribers
std::unordered_map<std::string, std::vector<std::shared_ptr<WsSubscriber>>> g_ws_subs;
std::mutex g_ws_mtx;

void send_locked(WsSubscriber& sub, const std::string& msg) {
    std::lock_guard<std::mutex> lk{sub.send_mtx};
    if (!sub.conn->isOpen()) return;
    try { sub.conn->sendText(msg); }
    catch (const std::exception& e) {
        std::cerr << "[ws] sendText failed (" << e.what() << ") — closing conn\n";
        try { sub.conn->close(); } catch (...) {}
    }
}

}  // namespace

void ws_run_publish(const std::string& run_id, std::string_view event_json) {
    std::vector<std::shared_ptr<WsSubscriber>> subs;
    {
        std::lock_guard<std::mutex> lk{g_ws_mtx};
        auto it = g_ws_subs.find(run_id);
        if (it != g_ws_subs.end()) subs = it->second;  // copy under lock
    }
    if (subs.empty()) return;
    // Trim trailing newline — each WS message is exactly one event JSON.
    std::string msg{event_json};
    while (!msg.empty() && (msg.back() == '\n' || msg.back() == '\r')) msg.pop_back();
    for (auto& s : subs) {
        try { send_locked(*s, msg); } catch (...) {}
    }
}

void register_ws_run_routes(OmniNet::HTTP::Server& server) {
    OmniNet::HTTP::Server::ProtocolSwitchHandlers handlers;

    // ── on_upgrade: validate + 101 (mirrors WebSocket::Server::on) ──────
    handlers.on_upgrade = [](const OmniNet::HTTP::Request& req,
                             OmniNet::HTTP::Response& resp) {
        auto upgrade = req.header("Upgrade");
        if (!upgrade || OmniNet::Utils::toLower(*upgrade) != "websocket") {
            resp = OmniNet::HTTP::Response::badRequest("WebSocket upgrade required");
            return;
        }
        auto key = req.header("Sec-WebSocket-Key");
        if (!key) {
            resp = OmniNet::HTTP::Response::badRequest("Missing Sec-WebSocket-Key");
            return;
        }
        auto acceptKey = OmniNet::WebSocket::Handshake::computeAcceptKey(std::string(*key));
        resp.setStatus(static_cast<OmniNet::HTTP::StatusCode>(101));
        resp.setHeader("Upgrade", std::string("websocket"));
        resp.setHeader("Connection", std::string("Upgrade"));
        resp.setHeader("Sec-WebSocket-Accept", acceptKey);
        resp.markProtocolSwitch();
    };

    // ── on_post_upgrade: own the socket, serve subscribe/stream ─────────
    handlers.on_post_upgrade = [](std::unique_ptr<OmniNet::Socket> socket,
                                  const OmniNet::HTTP::Request& req) {
        auto conn = std::make_shared<OmniNet::WebSocket::Connection>(
            std::move(socket), req.path());
        std::thread([conn]() {
            auto self = std::make_shared<WsSubscriber>();
            self->conn = conn;
            std::vector<std::string> my_runs;   // run_ids this conn subscribed to
            try {
                while (conn->isOpen()) {
                    auto msg = conn->receiveText();
                    if (!msg) break;   // closed / error
                    auto j = json::parse(*msg, nullptr, false);
                    if (j.is_discarded()) continue;
                    if (j.value("type", "") != "subscribe") continue;
                    std::string rid = j.value("run_id", "");
                    if (rid.empty()) continue;

                    {
                        std::lock_guard<std::mutex> lk{g_ws_mtx};
                        g_ws_subs[rid].push_back(self);
                    }
                    my_runs.push_back(rid);

                    // Backfill buffered events so late joiners catch up.
                    std::string snapshot = run_output_snapshot(rid);
                    std::istringstream iss{snapshot};
                    std::string line;
                    while (std::getline(iss, line)) {
                        if (!line.empty()) {
                            try { send_locked(*self, line); } catch (...) { break; }
                        }
                    }
                    try {
                        send_locked(*self, json{{"type", "subscribed"},
                                                {"run_id", rid}}.dump());
                    } catch (...) { break; }
                }
            } catch (...) {}

            // Unregister all subscriptions for this connection.
            std::lock_guard<std::mutex> lk{g_ws_mtx};
            for (auto& rid : my_runs) {
                auto it = g_ws_subs.find(rid);
                if (it == g_ws_subs.end()) continue;
                std::erase_if(it->second,
                              [&](auto& p) { return p.get() == self.get(); });
                if (it->second.empty()) g_ws_subs.erase(it);
            }
        }).detach();
    };

    server.registerProtocolSwitch("/ws/runs", std::move(handlers));
}
