// SPDX-License-Identifier: BSL-1.0
// include/hive-brain/a2a/A2aServer.h
//
// Public A2A (Agent2Agent Protocol) HTTP+JSON/REST server.
//
// Hosts implement IA2aTaskBackend to provide the agent card and to handle
// task lifecycle. A2aServer owns the OmniNet HTTP server, routes the
// spec-compliant endpoints, serializes JSON request/response bodies, and
// produces Server-Sent Events for streaming endpoints.

#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "a2a/types.h"

namespace hive::a2a {

/// @brief Host-implemented backend for the A2A server.
///
/// The server translates incoming HTTP requests into calls on this interface
/// and translates the results back into A2A JSON/SSE responses. Hosts can
/// delegate to hive::Agent instances, bridge to another framework, or
/// return canned responses for testing.
class IA2aTaskBackend {
public:
    virtual ~IA2aTaskBackend() = default;

    /// @brief Return the Agent Card served at /.well-known/agent-card.json.
    [[nodiscard]] virtual AgentCard get_agent_card() const = 0;

    /// @brief Handle POST /v1/message:send.
    [[nodiscard]] virtual A2aResult<SendMessageResult> send_message(
        const SendMessageRequest& request) = 0;

    /// @brief Handle GET /v1/tasks/{id}.
    [[nodiscard]] virtual A2aResult<Task> get_task(
        const GetTaskRequest& request) = 0;

    /// @brief Handle GET /v1/tasks.
    [[nodiscard]] virtual A2aResult<ListTasksResponse> list_tasks(
        const ListTasksRequest& request) = 0;

    /// @brief Handle POST /v1/tasks/{id}:cancel.
    [[nodiscard]] virtual A2aResult<Task> cancel_task(
        const CancelTaskRequest& request) = 0;

    /// @brief Handle POST /v1/message:stream.
    ///
    /// The backend calls the supplied handler with A2A stream events
    /// (task, statusUpdate, artifactUpdate, message). The server will wrap
    /// these in SSE frames.
    [[nodiscard]] virtual A2aResult<void> send_streaming_message(
        const SendMessageRequest& request,
        IA2aEventHandler& handler) = 0;

    /// @brief Handle POST /v1/tasks/{id}:subscribe.
    [[nodiscard]] virtual A2aResult<void> subscribe_to_task(
        const SubscribeToTaskRequest& request,
        IA2aEventHandler& handler) = 0;
};

/// @brief Configuration for A2aServer.
struct A2aServerConfig {
    /// @brief Bind address. Default: loopback only.
    std::string address = "127.0.0.1";

    /// @brief Bind port. 0 means an OS-assigned ephemeral port.
    uint16_t port = 0;

    /// @brief Required task backend.
    std::shared_ptr<IA2aTaskBackend> backend;

    /// @brief Number of OmniNet HTTP worker threads.
    int worker_count = 2;
};

/// @brief A2A HTTP+JSON/REST server.
///
/// The server is not copyable. start() blocks until stop() is called (from
/// another thread or signal handler). After construction, port() returns 0
/// until the server has bound; call start() to bind and run.
class A2aServer {
public:
    explicit A2aServer(A2aServerConfig cfg);
    ~A2aServer();

    A2aServer(const A2aServer&) = delete;
    A2aServer& operator=(const A2aServer&) = delete;

    /// @brief Bind, run the HTTP accept loop, and block until stop().
    void start();

    /// @brief Request the server to shut down gracefully.
    void stop();

    /// @brief Bound port. 0 before start() completes.
    [[nodiscard]] uint16_t port() const;

    /// @brief Convenience URL for the bound endpoint, e.g. http://127.0.0.1:8080.
    [[nodiscard]] std::string base_url() const;

    /// @brief True while the accept loop is running.
    [[nodiscard]] bool running() const;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace hive::a2a
