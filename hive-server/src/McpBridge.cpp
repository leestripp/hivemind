// src/McpBridge.cpp — Phase 33: MCP streamable-http → worker channel tunnel.
// See McpBridge.h for the model.

#include "hivemind/McpBridge.h"
#include "hivemind/WorkerChannel.h"

#include <OmniNet.h>

#include <iostream>

using json = nlohmann::json;
using OmniNet::HTTP::Request;
using OmniNet::HTTP::Response;
using OmniNet::HTTP::StatusCode;

namespace {

// Split "/mcp/<agent>/<server>" into segments (no pathParam — OmniNet's
// multi-param lookup is unreliable, known issue; parse manually).
std::vector<std::string> path_segments(const std::string& p) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : p) {
        if (c == '/') { if (!cur.empty()) { out.push_back(cur); cur.clear(); } }
        else cur += c;
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

void send_json(Response& resp, StatusCode code, const json& j) {
    resp.setStatus(code);
    resp.setContentType("application/json");
    resp.setBody(j.dump());
}

} // namespace

McpBridge::McpBridge(WorkerChannel& channel, int port)
    : channel_(channel), port_(port) {
    OmniNet::HTTP::Server::Config cfg;
    cfg.address = "127.0.0.1";   // loopback ONLY — daedalus runs on this host
    cfg.port = static_cast<uint16_t>(port_);
    cfg.logRequests = false;
    http_ = std::make_unique<OmniNet::HTTP::Server>(cfg);

    http_->post("/mcp", [this](const Request& req, Response& resp) {
        const auto seg = path_segments(std::string(req.path()));
        if (seg.size() != 3 || seg[0] != "mcp") {
            send_json(resp, StatusCode::NOT_FOUND,
                      json{{"error", "expected POST /mcp/<agent>/<server>"}});
            return;
        }
        const std::string agent = seg[1];
        const std::string server = seg[2];

        Spec spec;
        {
            std::lock_guard lk(m_);
            auto it = specs_.find({agent, server});
            if (it == specs_.end()) {
                send_json(resp, StatusCode::NOT_FOUND,
                          json{{"error", "no MCP spec registered for " +
                                          agent + "/" + server}});
                return;
            }
            spec = it->second;
        }

        const std::string payload{req.bodyAsString()};
        // Notifications (no JSON-RPC id) are fire-and-forget → 202.
        bool notification = false;
        try {
            json rpc = json::parse(payload);
            notification = !rpc.contains("id") || rpc["id"].is_null();
        } catch (...) {
            send_json(resp, StatusCode::BAD_REQUEST,
                      json{{"error", "invalid JSON-RPC payload"}});
            return;
        }

        auto r = channel_.mcp_rpc(spec.worker, agent, server, spec.command,
                                  spec.args, spec.env, payload,
                                  notification ? 10 : 120);
        if (notification) {
            // The worker wrote it to the process stdin; no reply expected.
            resp.setStatus(StatusCode::ACCEPTED);
            resp.setBody("");
            return;
        }
        if (!r) {
            send_json(resp, StatusCode::BAD_GATEWAY,
                      json{{"error", "worker offline"}});
            return;
        }
        if (!r->ok) {
            send_json(resp, StatusCode::BAD_GATEWAY,
                      json{{"error", r->error.empty() ? "mcp rpc failed"
                                                      : r->error}});
            return;
        }
        resp.setStatus(StatusCode::OK);
        resp.setContentType("application/json");
        resp.setBody(r->payload);
    });
}

McpBridge::~McpBridge() { stop(); }

void McpBridge::start() {
    thread_ = std::jthread([this] {
        std::cerr << "[mcp-bridge] listening on 127.0.0.1:" << port_ << "\n";
        http_->start();   // blocks in the accept loop
    });
}

void McpBridge::stop() {
    if (http_) http_->stop();
    if (thread_.joinable()) thread_.join();
}

void McpBridge::configure(const std::string& agent, const std::string& server,
                          const std::string& worker, std::string command,
                          std::vector<std::string> args, json env) {
    std::lock_guard lk(m_);
    specs_[{agent, server}] =
        Spec{worker, std::move(command), std::move(args), std::move(env)};
}

void McpBridge::remove_agent(const std::string& agent) {
    std::lock_guard lk(m_);
    for (auto it = specs_.begin(); it != specs_.end();)
        if (it->first.first == agent) it = specs_.erase(it);
        else ++it;
}
