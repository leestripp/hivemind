#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <span>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>
#include <unordered_map>

#include <nlohmann/json.hpp>

#include "net/Socket.h"   // ChannelSocket inherits OmniNet::Socket

namespace OmniNet::WebSocket { class Server; class Connection; }

class WorkerStore;

/**
 * WorkerChannel — Phase 30.2: the hive-worker WebSocket channel.
 *
 * Workers dial OUT to ws://<server>:<worker_ws_port>/api/workers/channel.
 * First frame must be a hello {"op":"hello","worker","key","caps","proto"};
 * the key is validated against WorkerStore (bad key → error frame + close,
 * proto mismatch → error frame + close). After that the connection is
 * tracked by worker name: is_online() drives the roster/API `online` flag,
 * send() (30.3) pushes exec frames down.
 *
 * Lifecycle: constructed with the store + port; start() runs the WS
 * server (its own accept/handler threads live inside OmniNet); stop()
 * shuts it down. Handler threads block in a receive loop per connection
 * and unregister on close.
 */
class WorkerChannel {
public:
    WorkerChannel(WorkerStore& store, int port, std::string bind = "0.0.0.0");
    ~WorkerChannel();

    void start();
    void stop();

    bool is_online(const std::string& name) const;

    /// Phase 30.3: synchronous remote exec. Sends an exec frame
    /// (argv = ["bash","-lc",command]; env_content is the RENDERED env-file
    /// text, resolved server-side — secrets never persist on the worker),
    /// collects streamed exec_chunk frames, returns on exec_done.
    /// nullopt when the worker is offline; throws std::runtime_error on
    /// worker-side failure (timeout included — exit_code -1, timed_out).
    struct RemoteResult { int exit_code = -1; std::string output;
                          bool timed_out = false; int64_t duration_ms = 0; };
    std::optional<RemoteResult> exec(const std::string& worker,
                                     const std::string& agent,
                                     const std::string& command,
                                     const std::string& env_content,
                                     int timeout_s);

    /// Push a JSON frame to a connected worker (30.3 exec dispatch).
    /// False when the worker is offline.
    bool send(const std::string& name, const nlohmann::json& frame);

    /// Phase 31: synchronous file fetch from a worker's shared workspace.
    /// Sends file_fetch {agent, path} (path is the agent-visible
    /// /workspace-relative path); the worker reads it from ITS host
    /// workspace dir and streams chunked base64 file_data frames, then
    /// file_done (or file_error). Data comes back DECODED (raw bytes).
    /// nullopt when the worker is offline; ok=false + error on failure.
    struct FileResult { bool ok = false; std::string data;
                        std::string error; int64_t size = 0; };
    std::optional<FileResult> fetch_file(const std::string& worker,
                                         const std::string& agent,
                                         const std::string& path,
                                         int timeout_s = 120);

    /// Phase 30.4: synchronous screen control on the worker (the worker
    /// runs the podman steps ContainerManager runs locally). image +
    /// resolution are passed through from server config.
    /// nullopt when the worker is offline; error field set on failure.
    struct ScreenResult { bool running = false; uint16_t port = 0;
                          std::string password; std::string error; };
    std::optional<ScreenResult> screen_ctl(const std::string& worker,
                                           const std::string& agent,
                                           const std::string& action,
                                           const std::string& image,
                                           const std::string& resolution,
                                           int timeout_s = 60);

    /// Phase 30.4: TCP stream over the channel. open_stream blocks until
    /// the worker acknowledges (stream_opened) or fails (stream_error /
    /// offline → ""). stream_send/stream_close drive it; inbound bytes
    /// arrive via on_data (called from the WS handler thread — keep it
    /// fast), EOF/error via on_close (exactly once).
    using StreamDataCb = std::function<void(std::string_view)>;
    using StreamCloseCb = std::function<void()>;
    std::string open_stream(const std::string& worker, uint16_t port,
                            StreamDataCb on_data, StreamCloseCb on_close);
    bool stream_send(const std::string& sid,
                     std::span<const uint8_t> data);
    void stream_close(const std::string& sid);

    /// Phase 33: push a skill directory to a worker (server→worker).
    /// Reads <skills_root>/<skill>/ on the SERVER and streams it as
    /// skill_begin / skill_file (192KB base64 chunks, eof flag) / skill_end;
    /// the worker writes ~/.local/share/hive-worker/skills/<skill>/ and
    /// links home-<agent>/.agents/skills/<skill> → /skills/<skill>, then
    /// acks (skill_ack). nullopt when offline; ok=false + error on failure.
    struct PushResult { bool ok = false; std::string error;
                        int files = 0; int64_t bytes = 0; };
    std::optional<PushResult> push_skill(const std::string& worker,
                                         const std::string& agent,
                                         const std::string& skill,
                                         int timeout_s = 180);
    /// Phase 33 D: push a file into the worker's shared workspace
    /// (server→worker; the reverse of fetch_file). rel_path is
    /// /workspace-relative; the worker confines it under
    /// ~/.local/share/hive-worker/workspace/. Chunked base64 frames
    /// (first/eof flags), acked via file_push_ack. nullopt offline.
    std::optional<PushResult> push_file(const std::string& worker,
                                        const std::string& rel_path,
                                        const std::string& data,
                                        int timeout_s = 120);

    /// Phase 33: remove an enabled skill on the worker (unlink + delete the
    /// rig-local copy).
    std::optional<PushResult> remove_skill(const std::string& worker,
                                           const std::string& agent,
                                           const std::string& skill,
                                           int timeout_s = 30);

    /// Phase 33: synchronous MCP JSON-RPC tunnel to a worker. The worker
    /// lazily spawns `command args...` (env) INSIDE hive-<agent> via
    /// podman exec -i and bridges stdio NDJSON. payload = one JSON-RPC
    /// request text; reply payload = the response line. nullopt offline.
    struct McpResult { bool ok = false; std::string payload;
                       std::string error; };

    /// Phase 33: fired after a worker's hello is accepted (reconcile hook —
    /// main.cpp uses it to re-push enabled skills after worker downtime).
    std::function<void(const std::string& worker)> on_worker_online;

    /// Aggregated model catalog across connected workers:
    /// [{"name","provider","worker"}] deduped by name.
    nlohmann::json models_catalog() const;

    /// Phase C: heartbeat frames carry per-agent states (v2 fleet). Fired
    /// with the worker name + the agents object {agent_id: state}.
    std::function<void(const std::string& worker,
                       const nlohmann::json& agents)> on_heartbeat;

    /// Hook for frames that are not channel bookkeeping (30.3: exec_chunk,
    /// exec_done, status). Called with the worker name + parsed frame.
    std::function<void(const std::string& worker,
                       const nlohmann::json& frame)> on_frame;

    static constexpr int kProto = 1;   // protocol major; hello must match

private:
    void handle(OmniNet::WebSocket::Connection& ws);

    WorkerStore& store_;
    int port_;
    std::unique_ptr<OmniNet::WebSocket::Server> ws_;

    mutable std::mutex m_;
    std::unordered_map<std::string, OmniNet::WebSocket::Connection*> conns_;
    /// worker name -> hello models catalog (json string), erased on disconnect.
    std::unordered_map<std::string, std::string> models_;

    // In-flight remote execs, keyed by job id (filled by exec_chunk /
    // exec_done frames in the receive loop).
    struct Job {
        std::mutex m;
        std::condition_variable cv;
        std::string output;
        int exit_code = -1;
        int64_t duration_ms = 0;
        bool done = false;
    };
    std::unordered_map<std::string, std::shared_ptr<Job>> jobs_;
    uint64_t next_job_ = 1;

    // 33: in-flight MCP rpcs (id → reply slot)

    // 33: in-flight skill pushes (id → ack slot)
    struct SkillJob {
        std::mutex m;
        std::condition_variable cv;
        bool done = false;
        PushResult result;
    };
    std::unordered_map<std::string, std::shared_ptr<SkillJob>> skill_jobs_;

    // 30.4: in-flight screen_ctl requests (id → result slot)
    struct ScreenJob {
        std::mutex m;
        std::condition_variable cv;
        bool done = false;
        ScreenResult result;
    };
    std::unordered_map<std::string, std::shared_ptr<ScreenJob>> screen_jobs_;

    // 31: file_fetch jobs (id → waiter)
    struct FileJob {
        std::mutex m;
        std::condition_variable cv;
        bool done = false;
        std::string b64;        // accumulated base64 chunks
        std::string error;
        int64_t size = 0;
    };
    std::unordered_map<std::string, std::shared_ptr<FileJob>> file_jobs_;

    // 30.4: stream_open acknowledgements (sid → waiter)
    struct StreamAck {
        std::mutex m;
        std::condition_variable cv;
        bool done = false;
        bool ok = false;
        std::string error;
    };
    std::unordered_map<std::string, std::shared_ptr<StreamAck>> stream_acks_;

    // 30.4: open streams (sid → callbacks + worker name)
    struct Stream {
        std::string worker;
        StreamDataCb on_data;
        StreamCloseCb on_close;
    };
    std::unordered_map<std::string, std::shared_ptr<Stream>> streams_;
    uint64_t next_stream_ = 1;
};

/**
 * ChannelSocket — Phase 30.4: an OmniNet::Socket facade over a worker
 * channel stream. Lets the existing screen splice code (raw pump +
 * upstream websockify handshake) run unchanged against a REMOTE
 * container: bytes written here ride stream_data frames to the worker,
 * which relays them to 127.0.0.1:<port>; inbound frames fill the recv
 * queue. recv() blocks (condition variable) and returns 0 on EOF,
 * matching Socket's "0 = closed" convention.
 *
 * The inbound callbacks capture a shared_ptr<State>, NOT `this` — a
 * frame in flight on the WS handler thread while the socket is being
 * destroyed then writes into a dead state block instead of freed memory.
 */
class ChannelSocket final : public OmniNet::Socket {
public:
    /// Opens the stream in the constructor; throws
    /// OmniNet::SocketException when the worker is offline or the worker
    /// fails to connect to the port.
    ChannelSocket(WorkerChannel& ch, const std::string& worker,
                  uint16_t port);
    ~ChannelSocket() override;

    // Socket interface — only the fd-backed primitives need overrides;
    // recvAll/recvExactly ride on virtual recv().
    size_t send(std::span<const uint8_t> data) override;
    size_t send(std::string_view data) override;
    void sendAll(std::span<const uint8_t> data) override;
    size_t recv(std::span<uint8_t> buffer) override;
    std::optional<std::string> readLine(size_t maxLength = 8192) override;
    bool isOpen() const override;
    void close() override;
    void shutdown(int how);   // base is non-virtual; teardown via close()
    bool waitForRead(std::chrono::milliseconds timeout) const override;

private:
    struct State {
        mutable std::mutex m;
        mutable std::condition_variable cv;
        std::string buf;        // pending inbound bytes
        bool eof = false;
    };

    WorkerChannel& ch_;
    std::string sid_;
    std::shared_ptr<State> st_;
};
