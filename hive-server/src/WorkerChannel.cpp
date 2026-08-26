// src/WorkerChannel.cpp — Phase 30.2: hive-worker WebSocket channel.
// See WorkerChannel.h for the model.

#include "hivemind/WorkerChannel.h"
#include "hivemind/WorkerStore.h"

#include "websocket/WebSocket.h"

#include <chrono>
#include <iostream>
#include <stdexcept>

using json = nlohmann::json;
using OmniNet::WebSocket::Connection;

WorkerChannel::WorkerChannel(WorkerStore& store, int port)
    : store_(store), port_(port) {
    OmniNet::HTTP::Server::Config cfg;
    cfg.address = "0.0.0.0";   // workers dial in from other machines
    cfg.port = static_cast<uint16_t>(port_);
    cfg.logRequests = false;
    // CRITICAL: OmniNet applies keepAliveTimeout (default 5s) as
    // SO_RCVTIMEO on the socket, and the upgraded WebSocket connection
    // INHERITS it — 5s of worker silence (e.g. a long podman create
    // during exec dispatch) looked like "peer closed" and the channel
    // dropped mid-exec. 0 disables the timeout: SO_RCVTIMEO {0,0} =
    // blocking reads, which is exactly right for a long-lived channel.
    cfg.keepAliveTimeout = std::chrono::seconds{0};
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

std::optional<WorkerChannel::RemoteResult> WorkerChannel::exec(
        const std::string& worker, const std::string& agent,
        const std::string& command, const std::string& env_content,
        int timeout_s) {
    auto job = std::make_shared<Job>();
    std::string id;
    {
        std::lock_guard lk(m_);
        if (!conns_.count(worker)) return std::nullopt;   // offline
        id = std::to_string(next_job_++);
        jobs_[id] = job;
    }
    bool sent = send(worker, json{{"op","exec"},{"id",id},{"agent",agent},
        {"argv", json::array({"bash","-lc",command})},
        {"env", env_content},           // rendered env-file text (may be "")
        {"timeout_s", timeout_s}});
    if (!sent) {
        std::lock_guard lk(m_);
        jobs_.erase(id);
        return std::nullopt;
    }

    // Wait for exec_done. Generous outer bound: worker timeout + overhead.
    std::unique_lock lk(job->m);
    bool ok = job->cv.wait_for(lk, std::chrono::seconds(timeout_s + 30),
                               [&] { return job->done; });
    RemoteResult r;
    r.exit_code = job->exit_code;
    r.output = std::move(job->output);
    r.duration_ms = job->duration_ms;
    r.timed_out = !ok || (ok && job->exit_code == -1 && job->done
                          && job->output.find("[timed out]") != std::string::npos);
    lk.unlock();
    {
        std::lock_guard g(m_);
        jobs_.erase(id);
    }
    if (!ok)
        throw std::runtime_error("worker exec timed out (no exec_done)");
    return r;
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
        } else if (op == "exec_chunk" || op == "exec_done") {
            std::shared_ptr<Job> job;
            {
                std::lock_guard lk(m_);
                auto it = jobs_.find(frame.value("id", ""));
                if (it != jobs_.end()) job = it->second;
            }
            if (job) {
                std::lock_guard jl(job->m);
                if (op == "exec_chunk") {
                    job->output += frame.value("data", "");
                } else {
                    job->exit_code = frame.value("exit_code", -1);
                    job->duration_ms = frame.value("duration_ms", 0);
                    job->done = true;
                    job->cv.notify_all();
                }
            }
        } else if (on_frame) {
            on_frame(name, frame);  // status / future ops
        }
    }

    {
        std::lock_guard lk(m_);
        conns_.erase(name);
        // Fail every in-flight job for this worker — exec() callers wake
        // with an error instead of hanging until the outer timeout.
        for (auto& [id, job] : jobs_) {
            std::lock_guard jl(job->m);
            if (!job->done) {
                job->output += "\n[worker disconnected]\n";
                job->exit_code = -1;
                job->done = true;
                job->cv.notify_all();
            }
        }
    }
    std::cerr << "[workers] " << name << " disconnected\n";
}
