// SPDX-License-Identifier: BSL-1.0
// include/hive-brain/a2a/A2aClient.h
//
// Public A2A (Agent2Agent Protocol) HTTP+JSON/REST client.
// Uses OmniNet for all HTTP and SSE transport.

#pragma once

#include <expected>
#include <memory>
#include <optional>
#include <string>

#include "a2a/types.h"

namespace hive::a2a {

class A2aClient;

/// Factory for creating an A2A client. Matches the style of the previous
/// AcpClient factory so hosts can swap namespaces with minimal churn.
[[nodiscard]] std::unique_ptr<A2aClient> create_a2a_client(
    std::string base_url,
    std::optional<std::string> bearer_token = std::nullopt,
    std::optional<std::string> tenant = std::nullopt);

class A2aClient {
public:
    A2aClient(std::string base_url,
              std::optional<std::string> bearer_token = std::nullopt,
              std::optional<std::string> tenant = std::nullopt);
    ~A2aClient() = default;

    A2aClient(const A2aClient&) = delete;
    A2aClient& operator=(const A2aClient&) = delete;
    A2aClient(A2aClient&&) = default;
    A2aClient& operator=(A2aClient&&) = default;

    void set_bearer_token(std::string token) { bearer_token_ = std::move(token); }
    void set_tenant(std::string tenant) { tenant_ = std::move(tenant); }

    [[nodiscard]] std::string base_url() const { return base_url_; }

    /// GET /.well-known/agent-card.json
    [[nodiscard]] A2aResult<AgentCard> get_agent_card() const;

    /// POST /message:send
    [[nodiscard]] A2aResult<SendMessageResult> send_message(
        const SendMessageRequest& req) const;

    /// POST /message:stream (SSE stream of StreamResponse objects)
    [[nodiscard]] A2aResult<void> send_streaming_message(
        const SendMessageRequest& req,
        IA2aEventHandler& handler) const;

    /// GET /tasks/{id}
    [[nodiscard]] A2aResult<Task> get_task(const GetTaskRequest& req) const;

    /// GET /tasks
    [[nodiscard]] A2aResult<ListTasksResponse> list_tasks(
        const ListTasksRequest& req) const;

    /// POST /tasks/{id}:cancel
    [[nodiscard]] A2aResult<Task> cancel_task(const CancelTaskRequest& req) const;

    /// GET /tasks/{id}:subscribe (SSE stream of StreamResponse objects)
    [[nodiscard]] A2aResult<void> subscribe_to_task(
        const SubscribeToTaskRequest& req,
        IA2aEventHandler& handler) const;

private:
    std::string base_url_;
    std::optional<std::string> bearer_token_;
    std::optional<std::string> tenant_;
};

} // namespace hive::a2a
