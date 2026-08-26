#pragma once

#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

#include <nlohmann/json.hpp>

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
    WorkerChannel(WorkerStore& store, int port);
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
};
