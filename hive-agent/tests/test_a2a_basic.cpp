// SPDX-License-Identifier: BSL-1.0
// tests/test_a2a_basic.cpp
//
// Hermetic unit tests for the public hive::a2a client. Spins up an
// in-process OmniNet HTTP server so no external network is required.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "OmniNet.h"
#include "http/HttpRequest.h"
#include "http/HttpResponse.h"
#include "http/HttpServer.h"

#include "library/Agent.h"
#include "api/ProviderPlugin.h"
#include "api/ProviderRegistry.h"
#include "library/model_discovery.h"
#include "a2a/A2aClient.h"

namespace a2a = hive::a2a;
using json = nlohmann::json;

class LocalA2aServer {
public:
    using Req = OmniNet::HTTP::Request;
    using Resp = OmniNet::HTTP::Response;

    explicit LocalA2aServer() {
        OmniNet::HTTP::Server::Config cfg;
        cfg.address = "127.0.0.1";
        cfg.port = 0;  // OS assigns ephemeral port
        cfg.workerCount = 2;

        server_ = std::make_unique<OmniNet::HTTP::Server>(cfg);

        // OmniNet's Router treats ':' as a path-parameter prefix, which
        // conflicts with the A2A colon endpoints (/message:send,
        // /tasks/{id}:cancel, etc.). Use a catch-all handler and dispatch
        // on the raw path/method ourselves.
        server_->router().any("*",
            [this](const Req& req, Resp& resp) { handle(req, resp); });

        server_->startAsync();

        // Wait briefly for the acceptor to bind.
        for (int i = 0; i < 50 && server_->boundPort() == 0; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        port_ = server_->boundPort();
    }

    ~LocalA2aServer() {
        server_->stop();
    }

    [[nodiscard]] std::string base_url() const {
        return "http://127.0.0.1:" + std::to_string(port_);
    }

private:
    void handle(const Req& req, Resp& resp) {
        using M = OmniNet::HTTP::Method;
        std::string path(req.path());

        if (req.method() == M::GET && path == "/.well-known/agent-card.json") {
            resp.setStatus(OmniNet::HTTP::StatusCode::OK);
            resp.setContentType("application/a2a+json");
            resp.setBody(make_agent_card().dump());
            return;
        }

        if (req.method() == M::POST && path == "/v1/message:send") {
            json wrapper;
            wrapper["task"] = make_task();
            resp.setStatus(OmniNet::HTTP::StatusCode::OK);
            resp.setContentType("application/a2a+json");
            resp.setBody(wrapper.dump());
            return;
        }

        if (req.method() == M::POST && path == "/v1/message:stream") {
            resp.setStatus(OmniNet::HTTP::StatusCode::OK);
            resp.setContentType("text/event-stream");
            std::string body;
            body += "data: " + json{{"task", make_task()}}.dump() + "\n\n";
            body += "data: " + json{{"statusUpdate", make_status_update()}}.dump() + "\n\n";
            body += "data: " + json{{"artifactUpdate", make_artifact_update()}}.dump() + "\n\n";
            resp.setBody(body);
            return;
        }

        if (req.method() == M::GET && path == "/v1/tasks") {
            json j;
            j["tasks"] = json::array({make_task()});
            j["nextPageToken"] = "";
            j["pageSize"] = 1;
            j["totalSize"] = 1;
            resp.setStatus(OmniNet::HTTP::StatusCode::OK);
            resp.setContentType("application/a2a+json");
            resp.setBody(j.dump());
            return;
        }

        if (req.method() == M::GET && path.starts_with("/v1/tasks/") && !path.ends_with(":subscribe")) {
            resp.setStatus(OmniNet::HTTP::StatusCode::OK);
            resp.setContentType("application/a2a+json");
            resp.setBody(make_task().dump());
            return;
        }

        if (req.method() == M::POST && path.ends_with(":cancel") && path.starts_with("/v1/tasks/")) {
            json t = make_task();
            t["status"]["state"] = "canceled";
            resp.setStatus(OmniNet::HTTP::StatusCode::OK);
            resp.setContentType("application/a2a+json");
            resp.setBody(t.dump());
            return;
        }

        if (req.method() == M::GET && path.ends_with(":subscribe") && path.starts_with("/v1/tasks/")) {
            resp.setStatus(OmniNet::HTTP::StatusCode::OK);
            resp.setContentType("text/event-stream");
            std::string body;
            body += "data: " + json{{"task", make_task()}}.dump() + "\n\n";
            body += "data: " + json{{"statusUpdate", make_status_update()}}.dump() + "\n\n";
            resp.setBody(body);
            return;
        }

        resp.setStatus(OmniNet::HTTP::StatusCode::NOT_FOUND);
    }

    [[nodiscard]] json make_agent_card() const {
        json card;
        card["name"] = "Test Agent";
        card["description"] = "A2A test server";
        json iface;
        iface["url"] = base_url() + "/v1";
        iface["protocolBinding"] = "HTTP+JSON";
        iface["protocolVersion"] = "1.0";
        card["supportedInterfaces"] = json::array({iface});
        card["capabilities"] = json::object();
        card["defaultInputModes"] = json::array({"text/plain"});
        card["defaultOutputModes"] = json::array({"text/plain"});
        card["skills"] = json::array();
        return card;
    }

    [[nodiscard]] json make_task() const {
        json status;
        status["state"] = "TASK_STATE_COMPLETED";
        status["timestamp"] = "2025-01-01T00:00:00Z";
        json task;
        task["id"] = "task-123";
        task["contextId"] = "ctx-456";
        task["status"] = status;
        json art;
        art["artifactId"] = "art-1";
        art["name"] = "answer";
        art["parts"] = json::array({json{{"text", "hello from a2a"}}});
        task["artifacts"] = json::array({art});
        return task;
    }

    [[nodiscard]] json make_status_update() const {
        json upd;
        upd["taskId"] = "task-123";
        upd["contextId"] = "ctx-456";
        json status;
        status["state"] = "TASK_STATE_WORKING";
        upd["status"] = status;
        return upd;
    }

    [[nodiscard]] json make_artifact_update() const {
        json upd;
        upd["taskId"] = "task-123";
        upd["contextId"] = "ctx-456";
        json art;
        art["artifactId"] = "art-2";
        art["parts"] = json::array({json{{"text", "chunk"}}});
        upd["artifact"] = art;
        upd["lastChunk"] = true;
        return upd;
    }

    std::unique_ptr<OmniNet::HTTP::Server> server_;
    uint16_t port_ = 0;
};

TEST_CASE("a2a get_agent_card") {
    LocalA2aServer srv;
    a2a::A2aClient client(srv.base_url());
    auto result = client.get_agent_card();
    REQUIRE(result.has_value());
    CHECK(result->name == "Test Agent");
    REQUIRE(!result->supported_interfaces.empty());
    CHECK(result->supported_interfaces.front().transport == "HTTP+JSON");
    CHECK(result->supported_interfaces.front().protocol_version == "1.0");
}

TEST_CASE("a2a send_message returns task") {
    LocalA2aServer srv;
    a2a::A2aClient client(srv.base_url());
    a2a::SendMessageRequest req;
    req.message.message_id = "msg-1";
    req.message.role = a2a::Role::user;
    req.message.parts.push_back(a2a::Part::from_text("hello"));
    auto result = client.send_message(req);
    REQUIRE(result.has_value());
    REQUIRE(result->task.has_value());
    CHECK(result->task->id == "task-123");
    CHECK(result->task->status.state == a2a::TaskState::completed);
    REQUIRE(!result->task->artifacts.empty());
    CHECK(result->task->artifacts.front().artifact_id == "art-1");
}

TEST_CASE("a2a get_task") {
    LocalA2aServer srv;
    a2a::A2aClient client(srv.base_url());
    a2a::GetTaskRequest req;
    req.id = "task-123";
    auto result = client.get_task(req);
    REQUIRE(result.has_value());
    CHECK(result->id == "task-123");
    CHECK(result->status.state == a2a::TaskState::completed);
}

TEST_CASE("a2a list_tasks") {
    LocalA2aServer srv;
    a2a::A2aClient client(srv.base_url());
    a2a::ListTasksRequest req;
    auto result = client.list_tasks(req);
    REQUIRE(result.has_value());
    CHECK(result->tasks.size() == 1);
    CHECK(result->total_size == 1);
}

TEST_CASE("a2a cancel_task") {
    LocalA2aServer srv;
    a2a::A2aClient client(srv.base_url());
    a2a::CancelTaskRequest req;
    req.id = "task-123";
    auto result = client.cancel_task(req);
    REQUIRE(result.has_value());
    CHECK(result->status.state == a2a::TaskState::canceled);
}

struct CountingHandler : a2a::IA2aEventHandler {
    int tasks = 0;
    int messages = 0;
    int status_updates = 0;
    int artifact_updates = 0;
    int completes = 0;

    void on_task(const a2a::Task&) override { ++tasks; }
    void on_message(const a2a::Message&) override { ++messages; }
    void on_status_update(const a2a::TaskStatusUpdateEvent&) override { ++status_updates; }
    void on_artifact_update(const a2a::TaskArtifactUpdateEvent&) override { ++artifact_updates; }
    void on_complete() override { ++completes; }
};

TEST_CASE("a2a send_streaming_message") {
    LocalA2aServer srv;
    a2a::A2aClient client(srv.base_url());
    a2a::SendMessageRequest req;
    req.message.message_id = "msg-2";
    req.message.role = a2a::Role::user;
    req.message.parts.push_back(a2a::Part::from_text("stream me"));
    CountingHandler handler;
    auto result = client.send_streaming_message(req, handler);
    REQUIRE(result.has_value());
    CHECK(handler.tasks == 1);
    CHECK(handler.status_updates == 1);
    CHECK(handler.artifact_updates == 1);
    CHECK(handler.completes == 1);
}

TEST_CASE("a2a subscribe_to_task") {
    LocalA2aServer srv;
    a2a::A2aClient client(srv.base_url());
    a2a::SubscribeToTaskRequest req;
    req.id = "task-123";
    CountingHandler handler;
    auto result = client.subscribe_to_task(req, handler);
    REQUIRE(result.has_value());
    CHECK(handler.tasks == 1);
    CHECK(handler.status_updates == 1);
    CHECK(handler.completes == 1);
}

TEST_CASE("a2a enums round-trip") {
    CHECK(a2a::to_string(a2a::TaskState::completed) == "TASK_STATE_COMPLETED");
    CHECK(a2a::task_state_from_string("working") == a2a::TaskState::working);
    CHECK(a2a::task_state_from_string("TASK_STATE_WORKING") == a2a::TaskState::working);
    CHECK(a2a::to_string(a2a::Role::user) == "ROLE_USER");
    CHECK(a2a::role_from_string("agent") == a2a::Role::agent);
    CHECK(a2a::role_from_string("ROLE_AGENT") == a2a::Role::agent);
}
