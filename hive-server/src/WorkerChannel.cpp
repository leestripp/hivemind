// src/WorkerChannel.cpp — Phase 30.2: hive-worker WebSocket channel.
// See WorkerChannel.h for the model.

#include <condition_variable>
#include <deque>
#include "hivemind/WorkerChannel.h"
#include <set>
#include "hivemind/WorkerStore.h"
#include "hivemind/AgentStore.h"

#include "websocket/WebSocket.h"
#include "crypto/Crypto.h"
#include "net/Socket.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <system_error>

namespace fs33 = std::filesystem;

using json = nlohmann::json;
using OmniNet::WebSocket::Connection;

WorkerChannel::WorkerChannel(WorkerStore& store, int port, std::string bind)
    : store_(store), port_(port) {
    OmniNet::HTTP::Server::Config cfg;
    // Workers dial in from other machines. Behind nginx (30.5) set
    // worker_ws_bind=127.0.0.1 in server.json — only nginx reaches it.
    cfg.address = std::move(bind);
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

// Per-connection state: an outbound queue drained by a dedicated writer
// thread. send() NEVER touches the socket -- a blocking sendText to a
// dead-but-undetected peer (NAT drop, kill -9: TCP retransmit stalls for
// minutes) must not hold the channel mutex, or every other worker's
// connect/send wedges behind it (dev-rig incident, 2026-08-30).
struct WorkerConn {
    OmniNet::WebSocket::Connection* ws = nullptr;   // owned by handler scope
    std::mutex qm;
    std::condition_variable qcv;
    std::deque<std::string> queue;
    bool dead = false;                              // under qm
    std::thread writer;
};

static void worker_writer(std::shared_ptr<WorkerConn> wc) {
    std::unique_lock lk(wc->qm);
    for (;;) {
        wc->qcv.wait(lk, [&] { return wc->dead || !wc->queue.empty(); });
        while (!wc->queue.empty()) {
            std::string msg = std::move(wc->queue.front());
            wc->queue.pop_front();
            lk.unlock();
            try { wc->ws->sendText(msg); }
            catch (...) {
                lk.lock();
                wc->dead = true;
                return;
            }
            lk.lock();
        }
        if (wc->dead) return;
    }
}

json WorkerChannel::models_catalog() const {
    // Aggregate hello catalogs across connected workers, dedup by name.
    json out = json::array();
    std::set<std::string> seen;
    std::lock_guard lk(m_);
    for (auto& [wname, cat] : models_) {
        json arr;
        try { arr = json::parse(cat); } catch (...) { continue; }
        for (auto& e : arr) {
            const std::string n = e.value("name", "");
            if (n.empty() || seen.count(n)) continue;
            seen.insert(n);
            out.push_back({{"name", n},
                           {"provider", e.value("provider", "")},
                           {"worker", wname}});
        }
    }
    return out;
}

void WorkerChannel::stop() {
    if (ws_ && ws_->isRunning()) ws_->stop();
}

std::vector<std::string> WorkerChannel::connected_workers() const {
    std::lock_guard lk(m_);
    std::vector<std::string> out;
    out.reserve(conns_.size());
    for (auto& [n, c] : conns_) out.push_back(n);
    return out;
}

bool WorkerChannel::is_online(const std::string& name) const {
    std::lock_guard lk(m_);
    return conns_.count(name) != 0;
}

bool WorkerChannel::send(const std::string& name, const json& frame) {
    std::shared_ptr<WorkerConn> wc;
    {
        std::lock_guard lk(m_);
        auto it = conns_.find(name);
        if (it == conns_.end()) return false;
        wc = it->second;   // shared_ptr keeps it alive past unlock
    }
    {
        std::lock_guard qlk(wc->qm);
        if (wc->dead) return false;
        wc->queue.push_back(frame.dump());
    }
    wc->qcv.notify_one();
    return true;
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
    r.duration_ms = job->duration_ms;
    // timed_out BEFORE the move (checked on job->output, not the moved-from husk)
    r.timed_out = !ok || (ok && job->exit_code == -1 && job->done
                          && job->output.find("[timed out]") != std::string::npos);
    r.output = std::move(job->output);
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
    // OmniNet's contract (v0.7.0): the handler registers callbacks and
    // RETURNS; OmniNet's own readLoop() then drives the connection. Do NOT
    // block in receiveText() here — Connection::receive() uses a per-call
    // buffer and silently drops coalesced frames, while readLoop() keeps
    // its leftover across reads. (Found 2026-08-26: last exec_chunk +
    // exec_done arriving in one TCP segment were lost.)

    // Per-connection state shared by the callbacks.
    struct State {
        bool authed = false;
        std::string name;
        std::string key;
        std::string models;   // hello catalog: [{name, provider}]
        std::shared_ptr<WorkerConn> conn;
    };
    auto st = std::make_shared<State>();

    ws.onText([this, &ws, st](std::string msg) {
        json frame;
        try { frame = json::parse(msg); }
        catch (...) { return; }

        if (!st->authed) {
            // ── first frame: hello + key validation ────────────────────
            if (frame.value("op", "") != "hello") {
                ws.sendText(R"({"op":"error","error":"first frame must be hello"})");
                ws.close();
                return;
            }
            if (frame.value("proto", 0) != kProto) {
                ws.sendText(json{{"op","error"},{"error","protocol mismatch"},
                                 {"server_proto",kProto}}.dump());
                ws.close();
                return;
            }
            st->name = frame.value("worker", "");
            st->key  = frame.value("key", "");
            st->models = frame.contains("models") ? frame["models"].dump()
                                                  : "[]";
            {
                std::lock_guard lk(m_);
                models_[st->name] = st->models;
            }
            const std::string caps =
                frame.contains("caps") ? frame["caps"].dump() : "{}";
            auto worker = store_.validate(st->key, caps, kProto);
            if (!worker || worker->name != st->name) {
                ws.sendText(R"({"op":"error","error":"bad worker key"})");
                ws.close();
                return;
            }
            st->authed = true;
            st->conn = std::make_shared<WorkerConn>();
            st->conn->ws = &ws;
            {
                std::lock_guard lk(m_);
                conns_[st->name] = st->conn;   // replaces any stale conn
            }
            std::cerr << "[workers] " << st->name << " connected\n";
            // welcome goes out BEFORE the writer starts (single-writer rule)
            ws.sendText(json{{"op","welcome"},{"worker",st->name},
                             {"proto",kProto}}.dump());
            st->conn->writer = std::thread(worker_writer, st->conn);
            if (on_worker_online) {
                // Reconcile off the handler thread — pushes block on acks.
                const std::string wname = st->name;
                std::thread([this, wname] {
                    try { on_worker_online(wname); } catch (...) {}
                }).detach();
            }
            return;
        }

        // ── authed frames ──────────────────────────────────────────────
        const std::string op = frame.value("op", "");
        if (op == "heartbeat") {
            store_.validate(st->key);   // touches last_seen
            if (on_heartbeat) {
                try { on_heartbeat(st->name, frame.value("agents",
                                        nlohmann::json::object())); }
                catch (...) {}
            }
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
        } else if (op == "file_chunk" || op == "file_done" ||
                   op == "file_error" || op == "file_list_result") {
            std::shared_ptr<FileJob> fj;
            {
                std::lock_guard lk(m_);
                auto it = file_jobs_.find(frame.value("id", ""));
                if (it != file_jobs_.end()) fj = it->second;
            }
            if (fj) {
                std::lock_guard jl(fj->m);
                if (op == "file_chunk") {
                    fj->b64 += frame.value("data", "");
                } else if (op == "file_list_result") {
                    fj->entries = frame.value("entries", json::array());
                    fj->done = true;
                    fj->cv.notify_all();
                } else if (op == "file_error") {
                    fj->error = frame.value("error", "unknown");
                    fj->done = true;
                    fj->cv.notify_all();
                } else {
                    fj->size = frame.value("size", 0);
                    fj->done = true;
                    fj->cv.notify_all();
                }
            }
        } else if (op == "skill_ack" || op == "file_push_ack") {
            std::shared_ptr<SkillJob> sj;
            {
                std::lock_guard lk(m_);
                auto it = skill_jobs_.find(frame.value("id", ""));
                if (it != skill_jobs_.end()) sj = it->second;
            }
            if (sj) {
                std::lock_guard jl(sj->m);
                sj->result.ok = frame.value("ok", false);
                sj->result.error = frame.value("error", "");
                sj->result.files = frame.value("files", 0);
                sj->result.bytes = frame.value("bytes", (int64_t)0);
                sj->done = true;
                sj->cv.notify_all();
            }
        } else if (op == "screen_result") {
            std::shared_ptr<ScreenJob> sj;
            {
                std::lock_guard lk(m_);
                auto it = screen_jobs_.find(frame.value("id", ""));
                if (it != screen_jobs_.end()) sj = it->second;
            }
            if (sj) {
                std::lock_guard jl(sj->m);
                sj->result.running = frame.value("running", false);
                sj->result.port = frame.value("port", 0);
                sj->result.password = frame.value("password", "");
                sj->result.error = frame.value("error", "");
                sj->done = true;
                sj->cv.notify_all();
            }
        } else if (op == "stream_opened" || op == "stream_error") {
            // open_stream waits on this via a ScreenJob-shaped slot? no —
            // streams have their own ack slot:
            std::shared_ptr<StreamAck> ack;
            {
                std::lock_guard lk(m_);
                auto it = stream_acks_.find(frame.value("sid", ""));
                if (it != stream_acks_.end()) ack = it->second;
            }
            if (ack) {
                std::lock_guard al(ack->m);
                ack->ok = (op == "stream_opened");
                ack->error = frame.value("error", "");
                ack->done = true;
                ack->cv.notify_all();
            }
        } else if (op == "stream_data" || op == "stream_close") {
            std::shared_ptr<Stream> stream;
            {
                std::lock_guard lk(m_);
                auto it = streams_.find(frame.value("sid", ""));
                if (it != streams_.end()) stream = it->second;
                if (op == "stream_close") streams_.erase(frame.value("sid", ""));
            }
            if (stream) {
                if (op == "stream_data") {
                    auto bytes = OmniNet::Crypto::base64Decode(
                        frame.value("b64", ""));
                    if (bytes && stream->on_data)
                        stream->on_data(std::string_view{
                            reinterpret_cast<const char*>(bytes->data()),
                            bytes->size()});
                } else if (stream->on_close) {
                    stream->on_close();
                }
            }
        } else if (on_frame) {
            on_frame(st->name, frame);  // status / future ops
        }
    });

    // NOTE onClose is only fired for CLEAN close frames (handleControlFrame);
    // an abrupt EOF (worker killed, network drop) just breaks the loop.
    // So: drive readLoop() OURSELVES and clean up after it returns — that
    // covers both paths. OmniNet runs readLoop() once more after this
    // handler returns; on an already-closed socket it exits immediately.
    ws.readLoop();

    if (st->authed) {
        if (st->conn) {
            {
                std::lock_guard qlk(st->conn->qm);
                st->conn->dead = true;
            }
            st->conn->qcv.notify_one();
            try { ws.close(); } catch (...) {}   // unblocks a stuck sendText
            if (st->conn->writer.joinable()) st->conn->writer.join();
        }
        std::lock_guard lk(m_);
        auto it = conns_.find(st->name);
        if (it != conns_.end() && it->second == st->conn) {
            conns_.erase(it);
            models_.erase(st->name);
        }
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
        std::cerr << "[workers] " << st->name << " disconnected\n";
    }
}

// ── 31: file fetch (worker → server) ────────────────────────────────────
std::optional<WorkerChannel::FileResult>
WorkerChannel::fetch_file(const std::string& worker, const std::string& agent,
                          const std::string& path, int timeout_s) {
    auto job = std::make_shared<FileJob>();
    std::string id;
    {
        std::lock_guard lk(m_);
        if (!conns_.count(worker)) return std::nullopt;   // offline
        id = "f" + std::to_string(next_job_++);
        file_jobs_[id] = job;
    }
    bool sent = send(worker, json{{"op","file_fetch"},{"id",id},
                                  {"agent",agent},{"path",path}});
    if (!sent) {
        std::lock_guard lk(m_);
        file_jobs_.erase(id);
        return std::nullopt;
    }

    std::unique_lock lk(job->m);
    bool done = job->cv.wait_for(lk, std::chrono::seconds(timeout_s),
                                 [&] { return job->done; });
    FileResult r;
    r.error = job->error;
    r.size = job->size;
    std::string b64 = std::move(job->b64);
    lk.unlock();
    {
        std::lock_guard g(m_);
        file_jobs_.erase(id);
    }
    if (!done) {
        r.ok = false;
        if (r.error.empty()) r.error = "worker file fetch timed out";
        return r;
    }
    if (!r.error.empty()) { r.ok = false; return r; }
    auto dec = OmniNet::Crypto::base64Decode(b64);
    if (!dec) { r.ok = false; r.error = "base64 decode failed"; return r; }
    r.data.assign(dec->begin(), dec->end());
    r.ok = true;
    return r;
}

// Phase H: workspace directory listing (worker to server)
std::optional<WorkerChannel::ListResult>
WorkerChannel::list_files(const std::string& worker, const std::string& path,
                          int timeout_s) {
    auto job = std::make_shared<FileJob>();
    std::string id;
    {
        std::lock_guard lk(m_);
        if (!conns_.count(worker)) return std::nullopt;   // offline
        id = "l" + std::to_string(next_job_++);
        file_jobs_[id] = job;
    }
    bool sent = send(worker, json{{"op","file_list"},{"id",id},
                                  {"path",path}});
    if (!sent) {
        std::lock_guard lk(m_);
        file_jobs_.erase(id);
        return std::nullopt;
    }

    std::unique_lock lk(job->m);
    bool done = job->cv.wait_for(lk, std::chrono::seconds(timeout_s),
                                 [&] { return job->done; });
    ListResult r;
    r.error = job->error;
    r.entries = std::move(job->entries);
    lk.unlock();
    {
        std::lock_guard g(m_);
        file_jobs_.erase(id);
    }
    if (!done) {
        r.ok = false;
        if (r.error.empty()) r.error = "worker file list timed out";
        return r;
    }
    if (!r.error.empty()) { r.ok = false; return r; }
    r.ok = true;
    return r;
}

// ── 30.4: screen control ────────────────────────────────────────────────
std::optional<WorkerChannel::ScreenResult>
WorkerChannel::screen_ctl(const std::string& worker, const std::string& agent,
                          const std::string& action, const std::string& image,
                          const std::string& resolution, int timeout_s) {
    if (!is_online(worker)) return std::nullopt;
    const std::string id = "sc-" + std::to_string(next_job_++);
    auto sj = std::make_shared<ScreenJob>();
    {
        std::lock_guard lk(m_);
        screen_jobs_[id] = sj;
    }
    if (!send(worker, {{"op","screen_ctl"},{"id",id},{"agent",agent},
                       {"action",action},{"image",image},
                       {"resolution",resolution}})) {
        std::lock_guard lk(m_);
        screen_jobs_.erase(id);
        return std::nullopt;
    }
    std::unique_lock lk(sj->m);
    if (!sj->cv.wait_for(lk, std::chrono::seconds(timeout_s),
                         [&]{ return sj->done; })) {
        std::lock_guard g(m_);
        screen_jobs_.erase(id);
        throw std::runtime_error("worker screen_ctl timed out");
    }
    {
        std::lock_guard g(m_);
        screen_jobs_.erase(id);
    }
    return sj->result;
}

// ── 30.4: streams ───────────────────────────────────────────────────────
std::string WorkerChannel::open_stream(const std::string& worker,
                                       uint16_t port,
                                       StreamDataCb on_data,
                                       StreamCloseCb on_close) {
    if (!is_online(worker)) return "";
    const std::string sid = "st-" + std::to_string(next_stream_++);
    auto ack = std::make_shared<StreamAck>();
    {
        std::lock_guard lk(m_);
        stream_acks_[sid] = ack;
    }
    if (!send(worker, {{"op","stream_open"},{"sid",sid},{"port",port}})) {
        std::lock_guard lk(m_);
        stream_acks_.erase(sid);
        return "";
    }
    {
        std::unique_lock lk(ack->m);
        if (!ack->cv.wait_for(lk, std::chrono::seconds(10),
                              [&]{ return ack->done; })) {
            std::lock_guard g(m_);
            stream_acks_.erase(sid);
            return "";
        }
    }
    {
        std::lock_guard lk(m_);
        stream_acks_.erase(sid);
        if (!ack->ok) return "";
        streams_[sid] = std::make_shared<Stream>(
            Stream{worker, std::move(on_data), std::move(on_close)});
    }
    return sid;
}

bool WorkerChannel::stream_send(const std::string& sid,
                                std::span<const uint8_t> data) {
    std::shared_ptr<Stream> stream;
    {
        std::lock_guard lk(m_);
        auto it = streams_.find(sid);
        if (it == streams_.end()) return false;
        stream = it->second;
    }
    return send(stream->worker,
                {{"op","stream_data"},{"sid",sid},
                 {"b64", OmniNet::Crypto::base64Encode(data)}});
}

void WorkerChannel::stream_close(const std::string& sid) {
    std::shared_ptr<Stream> stream;
    {
        std::lock_guard lk(m_);
        auto it = streams_.find(sid);
        if (it == streams_.end()) return;
        stream = it->second;
        streams_.erase(it);
    }
    send(stream->worker, {{"op","stream_close"},{"sid",sid}});
}

// ── 30.4: ChannelSocket ─────────────────────────────────────────────────
ChannelSocket::ChannelSocket(WorkerChannel& ch, const std::string& worker,
                             uint16_t port)
    : ch_(ch), st_(std::make_shared<State>()) {
    auto st = st_;   // callbacks capture the state block, not `this`
    sid_ = ch_.open_stream(worker, port,
        [st](std::string_view bytes) {
            {
                std::lock_guard lk(st->m);
                st->buf.append(bytes);
            }
            st->cv.notify_all();
        },
        [st] {
            {
                std::lock_guard lk(st->m);
                st->eof = true;
            }
            st->cv.notify_all();
        });
    if (sid_.empty())
        throw OmniNet::SocketException(
            std::string{"worker stream open failed"}, ECONNREFUSED);
}

ChannelSocket::~ChannelSocket() {
    if (isOpen()) {
        try { ch_.stream_close(sid_); } catch (...) {}
    }
}

size_t ChannelSocket::send(std::span<const uint8_t> data) {
    if (!isOpen()) return 0;
    return ch_.stream_send(sid_, data) ? data.size() : 0;
}

size_t ChannelSocket::send(std::string_view data) {
    return send(std::span<const uint8_t>(
        reinterpret_cast<const uint8_t*>(data.data()), data.size()));
}

void ChannelSocket::sendAll(std::span<const uint8_t> data) {
    if (send(data) != data.size())
        throw OmniNet::SocketException(
            std::string{"channel stream closed during send"});
}

size_t ChannelSocket::recv(std::span<uint8_t> buffer) {
    std::unique_lock lk(st_->m);
    st_->cv.wait(lk, [&]{ return !st_->buf.empty() || st_->eof; });
    if (st_->buf.empty()) return 0;   // EOF
    const size_t n = std::min(buffer.size(), st_->buf.size());
    std::memcpy(buffer.data(), st_->buf.data(), n);
    st_->buf.erase(0, n);
    return n;
}

std::optional<std::string> ChannelSocket::readLine(size_t maxLength) {
    // Base impl uses the raw fd — we must byte-pop our queue instead.
    std::string line;
    while (line.size() < maxLength) {
        uint8_t c = 0;
        if (recv(std::span<uint8_t>(&c, 1)) == 0)
            return line.empty() ? std::nullopt : std::optional{line};
        if (c == '\n') {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            return line;
        }
        line += static_cast<char>(c);
    }
    return line;
}

bool ChannelSocket::isOpen() const {
    std::lock_guard lk(st_->m);
    return !st_->eof;
}

void ChannelSocket::close() {
    {
        std::lock_guard lk(st_->m);
        if (st_->eof) return;
        st_->eof = true;
    }
    st_->cv.notify_all();
    try { ch_.stream_close(sid_); } catch (...) {}
}

void ChannelSocket::shutdown(int how) {
    // Half-close semantics for the splice: SHUT_WR = no more writes →
    // close the stream outright (the VNC splice only ever shuts down on
    // teardown anyway).
    if (how == SHUT_WR || how == SHUT_RDWR) close();
}

bool ChannelSocket::waitForRead(std::chrono::milliseconds timeout) const {
    std::unique_lock lk(st_->m);
    return st_->cv.wait_for(lk, timeout,
                            [&]{ return !st_->buf.empty() || st_->eof; });
}

// ─── Phase 33: skill sync (server → worker) ─────────────────────────────────
namespace {

fs33::path server_skills_root() {
    return AgentStore::agents_base_dir().parent_path() / "skills";
}

bool valid_skill_name33(const std::string& n) {
    if (n.empty() || n.size() > 64) return false;
    for (char c : n) {
        bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-';
        if (!ok) return false;
    }
    return true;
}

} // namespace

std::optional<WorkerChannel::PushResult>
WorkerChannel::push_skill(const std::string& worker, const std::string& agent,
                          const std::string& skill, int timeout_s) {
    PushResult res;
    if (!valid_skill_name33(skill)) {
        res.error = "bad skill name";
        return res;
    }
    const fs33::path root = server_skills_root() / skill;
    std::error_code ec;
    if (!fs33::is_directory(root, ec)) {
        res.error = "skill not in server library: " + skill;
        return res;
    }

    auto job = std::make_shared<SkillJob>();
    std::string id;
    {
        std::lock_guard lk(m_);
        if (!conns_.count(worker)) return std::nullopt;   // offline
        id = "s" + std::to_string(next_job_++);
        skill_jobs_[id] = job;
    }
    struct JobGuard {  // always drop the job slot
        WorkerChannel* ch; std::string id;
        ~JobGuard() { std::lock_guard lk(ch->m_); ch->skill_jobs_.erase(id); }
    } guard{this, id};

    if (!send(worker, json{{"op","skill_begin"},{"id",id},
                           {"agent",agent},{"skill",skill}})) {
        res.error = "worker offline mid-send";
        return res;
    }

    // Stream every regular file as 192KB base64 chunks with an eof flag.
    constexpr size_t kChunk = 192 * 1024;
    constexpr int64_t kCapBytes = 50ll * 1024 * 1024;
    int files = 0;
    int64_t bytes = 0;
    bool send_failed = false;
    for (auto it = fs33::recursive_directory_iterator(root, ec);
         !send_failed && it != fs33::recursive_directory_iterator();
         it.increment(ec)) {
        if (ec) break;
        if (!it->is_regular_file(ec)) continue;
        const fs33::path rel = it->path().lexically_relative(root);
        std::ifstream in(it->path(), std::ios::binary);
        if (!in) continue;
        std::string buf(kChunk, '\0');
        while (in) {
            in.read(buf.data(), (std::streamsize)buf.size());
            const auto got = (size_t)in.gcount();
            const bool eof = in.eof();
            if (got == 0 && !eof) break;
            bytes += (int64_t)got;
            if (bytes > kCapBytes) {
                send(worker, json{{"op","skill_abort"},{"id",id},
                                  {"error","skill too large (50MB cap)"}});
                res.error = "skill too large (50MB cap)";
                return res;
            }
            if (!send(worker, json{{"op","skill_file"},{"id",id},
                    {"path",rel.generic_string()},
                    {"data", OmniNet::Crypto::base64Encode(
                        std::span<const uint8_t>{
                            (const uint8_t*)buf.data(), got})},
                    {"eof",eof}})) {
                send_failed = true;
                break;
            }
            if (eof) break;
        }
        ++files;
    }
    if (send_failed) {
        res.error = "worker went offline mid-push";
        return res;
    }
    if (!send(worker, json{{"op","skill_end"},{"id",id},
                           {"files",files},{"bytes",bytes}})) {
        res.error = "worker offline at end";
        return res;
    }

    std::unique_lock lk(job->m);
    const bool done = job->cv.wait_for(lk, std::chrono::seconds(timeout_s),
                                       [&] { return job->done; });
    res = job->result;
    res.files = files;
    res.bytes = bytes;
    lk.unlock();
    if (!done) {
        res.ok = false;
        if (res.error.empty()) res.error = "worker skill push timed out";
    }
    return res;
}

std::optional<WorkerChannel::PushResult>
WorkerChannel::remove_skill(const std::string& worker, const std::string& agent,
                            const std::string& skill, int timeout_s) {
    PushResult res;
    if (!valid_skill_name33(skill)) { res.error = "bad skill name"; return res; }
    auto job = std::make_shared<SkillJob>();
    std::string id;
    {
        std::lock_guard lk(m_);
        if (!conns_.count(worker)) return std::nullopt;
        id = "s" + std::to_string(next_job_++);
        skill_jobs_[id] = job;
    }
    struct JobGuard {
        WorkerChannel* ch; std::string id;
        ~JobGuard() { std::lock_guard lk(ch->m_); ch->skill_jobs_.erase(id); }
    } guard{this, id};

    if (!send(worker, json{{"op","skill_remove"},{"id",id},
                           {"agent",agent},{"skill",skill}})) {
        res.error = "worker offline mid-send";
        return res;
    }
    std::unique_lock lk(job->m);
    const bool done = job->cv.wait_for(lk, std::chrono::seconds(timeout_s),
                                       [&] { return job->done; });
    res = job->result;
    lk.unlock();
    if (!done) { res.ok = false; res.error = "worker remove timed out"; }
    return res;
}


std::optional<WorkerChannel::PushResult>
WorkerChannel::push_file(const std::string& worker, const std::string& rel_path,
                         const std::string& data, int timeout_s) {
    PushResult res;
    if (rel_path.empty() || rel_path.find("..") != std::string::npos ||
        rel_path.front() == '/') {
        res.error = "bad path";
        return res;
    }
    auto job = std::make_shared<SkillJob>();
    std::string id;
    {
        std::lock_guard lk(m_);
        if (!conns_.count(worker)) return std::nullopt;
        id = "p" + std::to_string(next_job_++);
        skill_jobs_[id] = job;
    }
    struct JobGuard {
        WorkerChannel* ch; std::string id;
        ~JobGuard() { std::lock_guard lk(ch->m_); ch->skill_jobs_.erase(id); }
    } guard{this, id};

    constexpr size_t kChunk = 192 * 1024;
    size_t off = 0;
    bool first = true;
    do {
        const size_t n = std::min(kChunk, data.size() - off);
        const bool eof = (off + n) >= data.size();
        if (!send(worker, json{{"op","file_push"},{"id",id},
                {"path",rel_path},{"first",first},{"eof",eof},
                {"data", OmniNet::Crypto::base64Encode(
                    std::span<const uint8_t>{
                        (const uint8_t*)data.data() + off, n})}})) {
            res.error = "worker offline mid-push";
            return res;
        }
        off += n;
        first = false;
    } while (off < data.size() || (data.empty() && first));
    // empty file: the single frame above (first=true eof=true) covers it

    std::unique_lock lk(job->m);
    const bool done = job->cv.wait_for(lk, std::chrono::seconds(timeout_s),
                                       [&] { return job->done; });
    res = job->result;
    lk.unlock();
    if (!done) { res.ok = false; res.error = "worker file push timed out"; }
    if (res.bytes == 0) res.bytes = (int64_t)data.size();
    return res;
}
