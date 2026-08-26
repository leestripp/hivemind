// src/WorkerChannel.cpp — Phase 30.2: hive-worker WebSocket channel.
// See WorkerChannel.h for the model.

#include "hivemind/WorkerChannel.h"
#include "hivemind/WorkerStore.h"

#include "websocket/WebSocket.h"

#include <iostream>

using json = nlohmann::json;
using OmniNet::WebSocket::Connection;

WorkerChannel::WorkerChannel(WorkerStore& store, int port)
    : store_(store), port_(port) {
    OmniNet::HTTP::Server::Config cfg;
    cfg.address = "0.0.0.0";   // workers dial in from other machines
    cfg.port = static_cast<uint16_t>(port_);
    cfg.logRequests = false;
    ws_ = std::make_unique<OmniNet::WebSocket::Server>(cfg);
    ws_->on("/api/workers/channel", [this](Connection& ws) { handle(ws); });
}

WorkerChannel::~WorkerChannel() { stop(); }

void WorkerChannel::start() {
    ws_->start();
    std::cerr << "[workers] channel listening on :" << port_ << "\n";
}

void WorkerChannel::stop() {
    if (ws_ && ws_->isRunning()) ws_->stop();
}

bool WorkerChannel::is_online(const std::string& name) const {
    std::lock_guard lk(m_);
    return conns_.count(name) != 0;
}

bool WorkerChannel::send(const std::string& name, const json& frame) {
    std::lock_guard lk(m_);
    auto it = conns_.find(name);
    if (it == conns_.end()) return false;
    try {
        it->second->sendText(frame.dump());
        return true;
    } catch (...) {
        return false;   // receive loop will unregister on close
    }
}

void WorkerChannel::handle(Connection& ws) {
    // ── first frame: hello + key validation ────────────────────────────
    auto first = ws.receiveText();
    if (!first) return;
    json hello;
    try { hello = json::parse(*first); }
    catch (...) {
        ws.sendText(R"({"op":"error","error":"hello must be JSON"})");
        ws.close();
        return;
    }
    if (hello.value("op", "") != "hello") {
        ws.sendText(R"({"op":"error","error":"first frame must be hello"})");
        ws.close();
        return;
    }
    if (hello.value("proto", 0) != kProto) {
        ws.sendText(json{{"op","error"},{"error","protocol mismatch"},
                         {"server_proto",kProto}}.dump());
        ws.close();
        return;
    }
    const std::string name = hello.value("worker", "");
    const std::string key  = hello.value("key", "");
    const std::string caps = hello.contains("caps") ? hello["caps"].dump() : "{}";
    auto worker = store_.validate(key, caps, kProto);
    if (!worker || worker->name != name) {
        ws.sendText(R"({"op":"error","error":"bad worker key"})");
        ws.close();
        return;
    }

    {
        std::lock_guard lk(m_);
        conns_[name] = &ws;
    }
    std::cerr << "[workers] " << name << " connected\n";
    ws.sendText(json{{"op","welcome"},{"worker",name},{"proto",kProto}}.dump());

    // ── receive loop: bookkeeping inline, everything else to on_frame ──
    while (auto msg = ws.receiveText()) {
        json frame;
        try { frame = json::parse(*msg); }
        catch (...) { continue; }
        const std::string op = frame.value("op", "");
        if (op == "heartbeat") {
            store_.validate(key);   // touches last_seen
        } else if (on_frame) {
            on_frame(name, frame);  // exec_chunk / exec_done / status (30.3)
        }
    }

    {
        std::lock_guard lk(m_);
        conns_.erase(name);
    }
    std::cerr << "[workers] " << name << " disconnected\n";
}
