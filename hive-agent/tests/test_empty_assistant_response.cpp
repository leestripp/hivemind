/**
 * @file test_empty_assistant_response.cpp
 * @brief Regression for Forge's 2026-11-12 persisted-history blockage.
 *
 * The provider first finishes with `repeat` and no assistant content or
 * tool calls. The client must discard that response, make a valid recovery
 * request, and accept the following normal answer. Before the fix the empty
 * assistant was persisted and Kimi rejected every later request with 400.
 */

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <atomic>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include "api/LLMClient.h"
#include "library/AgentSink.h"

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {

class EmptyThenSuccessServer {
public:
    EmptyThenSuccessServer() {
        listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (listen_fd_ < 0) throw std::runtime_error("socket() failed");
        int yes = 1;
        ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;
        if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
            throw std::runtime_error("bind failed");
        }
        if (::listen(listen_fd_, 4) < 0) throw std::runtime_error("listen failed");

        socklen_t len = sizeof(addr);
        ::getsockname(listen_fd_, reinterpret_cast<sockaddr*>(&addr), &len);
        port_ = ntohs(addr.sin_port);
        running_.store(true);
        thread_ = std::thread([this] { loop(); });
    }

    ~EmptyThenSuccessServer() {
        running_.store(false);
        ::shutdown(listen_fd_, SHUT_RDWR);
        ::close(listen_fd_);
        if (thread_.joinable()) thread_.join();
    }

    std::string base_url() const {
        return "http://127.0.0.1:" + std::to_string(port_);
    }
    int request_count() const { return request_count_.load(); }
    bool rejected_empty_assistant() const { return rejected_empty_.load(); }

private:
    void loop() {
        while (running_.load()) {
            sockaddr_in client_addr{};
            socklen_t len = sizeof(client_addr);
            const int fd = ::accept(listen_fd_, reinterpret_cast<sockaddr*>(&client_addr), &len);
            if (fd < 0) break;
            handle_one(fd);
            ::close(fd);
        }
    }

    void handle_one(int fd) {
        std::string input;
        char buffer[8192];
        size_t content_length = 0;
        size_t body_start = std::string::npos;
        while (true) {
            const ssize_t count = ::recv(fd, buffer, sizeof(buffer), 0);
            if (count <= 0) return;
            input.append(buffer, static_cast<size_t>(count));
            body_start = input.find("\r\n\r\n");
            if (body_start != std::string::npos) {
                const auto header = input.find("Content-Length:");
                if (header != std::string::npos) {
                    content_length = static_cast<size_t>(
                        std::strtoul(input.c_str() + header + 15, nullptr, 10));
                }
                break;
            }
            if (input.size() > 1024 * 1024) return;
        }
        while (input.size() - (body_start + 4) < content_length) {
            const ssize_t count = ::recv(fd, buffer, sizeof(buffer), 0);
            if (count <= 0) break;
            input.append(buffer, static_cast<size_t>(count));
        }

        const int sequence = request_count_.fetch_add(1);
        const json request = json::parse(input.substr(body_start + 4));
        bool contains_empty = false;
        for (const auto& message : request.at("messages")) {
            contains_empty = contains_empty || Messages::is_empty_assistant_message(message);
        }

        json response;
        long status = 200;
        if (contains_empty) {
            rejected_empty_.store(true);
            status = 400;
            response = {
                {"error", {
                    {"type", "invalid_request_error"},
                    {"message", "the message with role 'assistant' must not be empty"},
                }},
            };
        } else if (sequence == 0) {
            response = sse_response("", "repeat");
        } else {
            response = sse_response("Recovered without poisoning history.", "stop");
        }

        const std::string body = response.is_string()
            ? response.get<std::string>() : response.dump();
        const std::string http =
            "HTTP/1.1 " + std::to_string(status) + (status == 200 ? " OK" : " Bad Request") +
            "\r\nContent-Type: " + (status == 200 ? "text/event-stream" : "application/json") +
            "\r\nContent-Length: " + std::to_string(body.size()) +
            "\r\nConnection: close\r\n\r\n" + body;
        ::send(fd, http.data(), http.size(), 0);
    }

    static json sse_response(const std::string& content, const std::string& finish) {
        const json first = {
            {"id", "chatcmpl-test"},
            {"choices", json::array({{
                {"index", 0},
                {"delta", {{"role", "assistant"}, {"content", content}}},
            }})},
        };
        const json last = {
            {"id", "chatcmpl-test"},
            {"choices", json::array({{
                {"index", 0}, {"delta", json::object()}, {"finish_reason", finish},
            }})},
            {"usage", {{"prompt_tokens", 10}, {"completion_tokens", 1}, {"total_tokens", 11}}},
        };
        return json("data: " + first.dump() + "\n\n" +
                    "data: " + last.dump() + "\n\n" +
                    "data: [DONE]\n\n");
    }

    int listen_fd_ = -1;
    int port_ = 0;
    std::atomic<bool> running_{false};
    std::atomic<bool> rejected_empty_{false};
    std::atomic<int> request_count_{0};
    std::thread thread_;
};

struct ScratchDir {
    fs::path path;
    ScratchDir() {
        path = fs::temp_directory_path() /
               ("hive_empty_assistant_test_" + std::to_string(::getpid()));
        fs::remove_all(path);
        fs::create_directories(path);
    }
    ~ScratchDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

} // namespace

TEST_CASE("empty repeat response is retried without entering provider history") {
    EmptyThenSuccessServer server;
    ScratchDir scratch;

    AppConfig config;
    config.base_url = server.base_url();
    config.chat_endpoint = "/v1/chat/completions";
    config.api_key = "test-key";
    config.model = "test-model";
    config.plugin = "openai-compatible";
    config.target_directory = scratch.path;
    config.memory_disabled = true;
    config.stream = true;

    LLMClient client(config);
    client.initialize();
    const AgentState final_state = client.run_one_turn("show the barrel preview");

    CHECK(final_state == AgentState::DONE);
    CHECK(server.request_count() == 2);
    CHECK(!server.rejected_empty_assistant());
    CHECK(client.get_last_assistant_text() == "Recovered without poisoning history.");
    CHECK(!client.ended_on_fatal_breaker());
    client.cleanup();
}
