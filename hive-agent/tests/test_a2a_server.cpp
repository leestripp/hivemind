// SPDX-License-Identifier: BSL-1.0
// tests/test_a2a_server.cpp
//
// Hermetic unit tests for the public hive::a2a::A2aServer. Spins up an
// in-process server with a canned backend and exercises it via A2aClient.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <thread>

#include "library/Agent.h"
#include "api/ProviderPlugin.h"
#include "api/ProviderRegistry.h"
#include "library/model_discovery.h"
#include "a2a/A2aClient.h"
#include "a2a/A2aClient.h"
#include "a2a/A2aServer.h"

namespace a2a = hive::a2a;
using json = nlohmann::json;

class CannedBackend : public a2a::IA2aTaskBackend {
public:
    [[nodiscard]] a2a::AgentCard get_agent_card() const override {
        a2a::AgentCard c;
        c.name = "Canned Agent";
        c.description = "Test backend";
        c.version = "1.0.0";
        a2a::AgentInterface iface;
        iface.url = "http://127.0.0.1:1/v1";
        iface.transport = "HTTP+JSON";
        iface.protocol_version = "1.0";
        c.supported_interfaces.push_back(std::move(iface));
        c.capabilities.streaming = true;
        c.default_input_modes = {"text/plain"};
        c.default_output_modes = {"text/plain"};
        return c;
    }

    [[nodiscard]] a2a::A2aResult<a2a::SendMessageResult> send_message(
        const a2a::SendMessageRequest& req) override {
        a2a::SendMessageResult r;
        r.task = make_task(req.message.parts);
        return r;
    }

    [[nodiscard]] a2a::A2aResult<a2a::Task> get_task(
        const a2a::GetTaskRequest& req) override {
        if (req.id != "task-123") {
            return std::unexpected(a2a::A2aError{404, "TASK_NOT_FOUND", "no such task", nullptr});
        }
        return make_task({});
    }

    [[nodiscard]] a2a::A2aResult<a2a::ListTasksResponse> list_tasks(
        const a2a::ListTasksRequest&) override {
        a2a::ListTasksResponse r;
        r.tasks.push_back(make_task({}));
        r.total_size = 1;
        r.page_size = 1;
        return r;
    }

    [[nodiscard]] a2a::A2aResult<a2a::Task> cancel_task(
        const a2a::CancelTaskRequest& req) override {
        if (req.id != "task-123") {
            return std::unexpected(a2a::A2aError{404, "TASK_NOT_FOUND", "no such task", nullptr});
        }
        auto t = make_task({});
        t.status.state = a2a::TaskState::canceled;
        return t;
    }

    [[nodiscard]] a2a::A2aResult<void> send_streaming_message(
        const a2a::SendMessageRequest& req,
        a2a::IA2aEventHandler& handler) override {
        handler.on_status_update(make_status_update(a2a::TaskState::working));
        handler.on_artifact_update(make_artifact_update("chunk-1", "hello ", false));
        handler.on_artifact_update(make_artifact_update("chunk-2", "world", true));
        handler.on_task(make_task(req.message.parts));
        return {};
    }

    [[nodiscard]] a2a::A2aResult<void> subscribe_to_task(
        const a2a::SubscribeToTaskRequest& req,
        a2a::IA2aEventHandler& handler) override {
        if (req.id != "task-123") {
            return std::unexpected(a2a::A2aError{404, "TASK_NOT_FOUND", "no such task", nullptr});
        }
        handler.on_task(make_task({}));
        return {};
    }

private:
    static a2a::Task make_task(const std::vector<a2a::Part>& parts) {
        a2a::Task t;
        t.id = "task-123";
        t.context_id = "ctx-456";
        t.status.state = a2a::TaskState::completed;
        a2a::Artifact a;
        a.artifact_id = "art-1";
        a.name = "answer";
        a.parts = parts.empty() ? std::vector<a2a::Part>{a2a::Part::from_text("hello")} : parts;
        t.artifacts.push_back(std::move(a));
        return t;
    }

    static a2a::TaskStatusUpdateEvent make_status_update(a2a::TaskState state) {
        a2a::TaskStatusUpdateEvent e;
        e.task_id = "task-123";
        e.context_id = "ctx-456";
        e.status.state = state;
        return e;
    }

    static a2a::TaskArtifactUpdateEvent make_artifact_update(std::string id,
                                                              std::string text,
                                                              bool last) {
        a2a::TaskArtifactUpdateEvent e;
        e.task_id = "task-123";
        e.context_id = "ctx-456";
        e.artifact.artifact_id = std::move(id);
        e.artifact.parts.push_back(a2a::Part::from_text(std::move(text)));
        e.last_chunk = last;
        return e;
    }
};

class ServerRunner {
public:
    explicit ServerRunner(std::shared_ptr<a2a::IA2aTaskBackend> backend) {
        a2a::A2aServerConfig cfg;
        cfg.address = "127.0.0.1";
        cfg.port = 0;
        cfg.backend = std::move(backend);
        cfg.worker_count = 2;

        server_ = std::make_unique<a2a::A2aServer>(std::move(cfg));
        thread_ = std::jthread([this] { server_->start(); });

        // Wait for the acceptor to bind.
        for (int i = 0; i < 100 && server_->port() == 0; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }

    ~ServerRunner() {
        server_->stop();
    }

    [[nodiscard]] std::string base_url() const { return server_->base_url(); }
    [[nodiscard]] uint16_t port() const { return server_->port(); }

private:
    std::unique_ptr<a2a::A2aServer> server_;
    std::jthread thread_;
};

TEST_CASE("a2a server serves agent card") {
    ServerRunner srv(std::make_shared<CannedBackend>());
    a2a::A2aClient client(srv.base_url());
    auto result = client.get_agent_card();
    REQUIRE(result.has_value());
    CHECK(result->name == "Canned Agent");
    CHECK(result->version == "1.0.0");
    REQUIRE(result->capabilities.streaming.has_value());
    CHECK(*result->capabilities.streaming == true);
}

TEST_CASE("a2a server send_message returns task") {
    ServerRunner srv(std::make_shared<CannedBackend>());
    a2a::A2aClient client(srv.base_url());
    a2a::SendMessageRequest req;
    req.message.message_id = "msg-1";
    req.message.role = a2a::Role::user;
    req.message.parts.push_back(a2a::Part::from_text("hi"));
    auto result = client.send_message(req);
    REQUIRE(result.has_value());
    REQUIRE(result->task.has_value());
    CHECK(result->task->id == "task-123");
    REQUIRE(!result->task->artifacts.empty());
    REQUIRE(result->task->artifacts.front().parts.front().text.has_value());
    CHECK(*result->task->artifacts.front().parts.front().text == "hi");
}

TEST_CASE("a2a server get_task and list_tasks") {
    ServerRunner srv(std::make_shared<CannedBackend>());
    a2a::A2aClient client(srv.base_url());

    a2a::GetTaskRequest get;
    get.id = "task-123";
    auto r1 = client.get_task(get);
    REQUIRE(r1.has_value());
    CHECK(r1->id == "task-123");

    a2a::ListTasksRequest list;
    auto r2 = client.list_tasks(list);
    REQUIRE(r2.has_value());
    CHECK(r2->tasks.size() == 1);
    CHECK(r2->total_size == 1);
}

TEST_CASE("a2a server cancel_task") {
    ServerRunner srv(std::make_shared<CannedBackend>());
    a2a::A2aClient client(srv.base_url());
    a2a::CancelTaskRequest req;
    req.id = "task-123";
    auto result = client.cancel_task(req);
    REQUIRE(result.has_value());
    CHECK(result->status.state == a2a::TaskState::canceled);
}

struct CountingHandler : a2a::IA2aEventHandler {
    int tasks = 0;
    int status_updates = 0;
    int artifact_updates = 0;

    void on_task(const a2a::Task&) override { ++tasks; }
    void on_status_update(const a2a::TaskStatusUpdateEvent&) override { ++status_updates; }
    void on_artifact_update(const a2a::TaskArtifactUpdateEvent&) override { ++artifact_updates; }
};

TEST_CASE("a2a server send_streaming_message") {
    ServerRunner srv(std::make_shared<CannedBackend>());
    a2a::A2aClient client(srv.base_url());
    a2a::SendMessageRequest req;
    req.message.message_id = "msg-2";
    req.message.role = a2a::Role::user;
    req.message.parts.push_back(a2a::Part::from_text("stream"));
    CountingHandler handler;
    auto result = client.send_streaming_message(req, handler);
    REQUIRE(result.has_value());
    CHECK(handler.status_updates == 1);
    CHECK(handler.artifact_updates == 2);
    CHECK(handler.tasks == 1);
}

TEST_CASE("a2a server subscribe_to_task") {
    ServerRunner srv(std::make_shared<CannedBackend>());
    a2a::A2aClient client(srv.base_url());
    a2a::SubscribeToTaskRequest req;
    req.id = "task-123";
    CountingHandler handler;
    auto result = client.subscribe_to_task(req, handler);
    REQUIRE(result.has_value());
    CHECK(handler.tasks == 1);
}

TEST_CASE("a2a server returns error for unknown task") {
    ServerRunner srv(std::make_shared<CannedBackend>());
    a2a::A2aClient client(srv.base_url());
    a2a::GetTaskRequest req;
    req.id = "missing";
    auto result = client.get_task(req);
    REQUIRE(!result.has_value());
    CHECK(result.error().http_status == 404);
}
