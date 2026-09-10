/**
 * @file test_status_only_breaker.cpp
 * @brief Regression tests for the set_status idle-loop bug (§IdleExitBreaker,
 *        2026-08-20).
 *
 * A finished model kept calling set_status("Done") expecting the agent loop
 * to exit — but set_status only *reports* state to the host; a turn ends
 * exclusively via the `stop` tool (or host cancel). The host re-invoked the
 * model with only the auto context-status block, the model replied with
 * another set_status, and the turn looped forever.
 *
 * The fix counts consecutive iterations whose ONLY tool call is set_status
 * and ends the turn (like stop) once the count reaches 2. These tests drive
 * LLMClient against an in-process HTTP server that answers chat completions
 * with a set_status tool call — mirroring test_fatal_breaker.cpp.
 */

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <atomic>
#include <cstring>
#include <filesystem>
#include <string>
#include <thread>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include "api/LLMClient.h"

namespace fs = std::filesystem;

namespace {

// ---------------------------------------------------------------------------
// In-process HTTP server that answers every POST /v1/chat/completions with a
// valid chat completion. Two modes:
//   ALL_STATUS       — every response is ONE set_status("Done") tool call
//   STATUS_THEN_TEXT — ALTERNATES per request: even seq = set_status tool
//                      call, odd seq = plain text (so each turn in the
//                      reset test runs status-then-text)
// Tool call ids are unique per request (call_0, call_1, ...).
// ---------------------------------------------------------------------------
class StatusOnlyServer {
public:
    enum Mode { ALL_STATUS, STATUS_THEN_TEXT };

    explicit StatusOnlyServer(Mode mode) : mode_(mode) {
        listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (listen_fd_ < 0) throw std::runtime_error("socket() failed");
        int yes = 1;
        ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;
        if (::bind(listen_fd_, (sockaddr*)&addr, sizeof(addr)) < 0)
            throw std::runtime_error("bind failed");
        if (::listen(listen_fd_, 4) < 0)
            throw std::runtime_error("listen failed");

        socklen_t len = sizeof(addr);
        ::getsockname(listen_fd_, (sockaddr*)&addr, &len);
        port_ = ntohs(addr.sin_port);

        running_.store(true);
        thread_ = std::thread([this] { loop(); });
    }

    ~StatusOnlyServer() {
        running_.store(false);
        ::shutdown(listen_fd_, SHUT_RDWR);
        ::close(listen_fd_);
        if (thread_.joinable()) thread_.join();
    }

    std::string base_url() const {
        return "http://127.0.0.1:" + std::to_string(port_);
    }
    int request_count() const { return request_count_.load(); }

private:
    void loop() {
        while (running_.load()) {
            sockaddr_in caddr{};
            socklen_t clen = sizeof(caddr);
            int cfd = ::accept(listen_fd_, (sockaddr*)&caddr, &clen);
            if (cfd < 0) break;
            handle_one(cfd);
            ::close(cfd);
        }
    }

    void handle_one(int cfd) {
        // Read headers, then drain the body per Content-Length.
        std::string in;
        char buf[8192];
        size_t content_length = 0;
        size_t body_start = std::string::npos;
        while (true) {
            ssize_t n = ::recv(cfd, buf, sizeof(buf), 0);
            if (n <= 0) return;
            in.append(buf, static_cast<size_t>(n));
            body_start = in.find("\r\n\r\n");
            if (body_start != std::string::npos) {
                auto cl = in.find("Content-Length:");
                if (cl != std::string::npos) {
                    content_length = static_cast<size_t>(
                        std::strtoul(in.c_str() + cl + 15, nullptr, 10));
                }
                break;
            }
            if (in.size() > 1024 * 1024) return;
        }
        while (in.size() - (body_start + 4) < content_length) {
            ssize_t n = ::recv(cfd, buf, sizeof(buf), 0);
            if (n <= 0) break;
            in.append(buf, static_cast<size_t>(n));
        }

        const int seq = request_count_.fetch_add(1);
        std::string body;
        if (mode_ == ALL_STATUS || (mode_ == STATUS_THEN_TEXT && seq % 2 == 0)) {
            body = status_tool_call_response(seq);
        } else {
            body = text_response();
        }

        const std::string resp =
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: application/json\r\n"
            "Content-Length: " + std::to_string(body.size()) + "\r\n"
            "Connection: close\r\n\r\n" + body;
        ::send(cfd, resp.data(), resp.size(), 0);
    }

    static std::string status_tool_call_response(int seq) {
        return std::string(
            R"({"id":"chatcmpl-test","object":"chat.completion","choices":[)"
            R"({"index":0,"message":{"role":"assistant","content":null,)"
            R"("tool_calls":[{"id":"call_)" + std::to_string(seq) +
            R"(","type":"function","function":{"name":"set_status",)"
            R"("arguments":"{\"state\": \"Done\"}"}}]},)"
            R"("finish_reason":"tool_calls"}],)"
            R"("usage":{"prompt_tokens":10,"completion_tokens":5,"total_tokens":15}})");
    }

    static std::string text_response() {
        return std::string(
            R"({"id":"chatcmpl-test","object":"chat.completion","choices":[)"
            R"({"index":0,"message":{"role":"assistant",)"
            R"("content":"All done."},"finish_reason":"stop"}],)"
            R"("usage":{"prompt_tokens":10,"completion_tokens":5,"total_tokens":15}})");
    }

    Mode mode_;
    int listen_fd_ = -1;
    int port_ = 0;
    std::atomic<bool> running_{false};
    std::atomic<int> request_count_{0};
    std::thread thread_;
};

struct ScratchDir {
    fs::path path;
    explicit ScratchDir(const std::string& tag) {
        path = fs::temp_directory_path() /
               ("hive_agent_test_" + tag + "_" +
                std::to_string(reinterpret_cast<std::uintptr_t>(this)));
        fs::remove_all(path);
        fs::create_directories(path);
    }
    ~ScratchDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

AppConfig make_config(const std::string& base_url, const fs::path& target) {
    AppConfig cfg;
    cfg.base_url = base_url;
    cfg.chat_endpoint = "/v1/chat/completions";
    cfg.api_key = "test-key";
    cfg.model = "test-model";
    cfg.plugin = "openai-compatible";  // loopback hosts auto-detect lmstudio
    cfg.target_directory = target;
    cfg.memory_disabled = true;
    cfg.stream = false;  // buffered JSON path — no SSE needed from the fake
    return cfg;
}

} // namespace

TEST_SUITE("set_status-only circuit breaker (§IdleExitBreaker)") {

    TEST_CASE("two consecutive set_status-only iterations end the turn") {
        StatusOnlyServer server(StatusOnlyServer::ALL_STATUS);
        ScratchDir scratch("status-only-breaker");

        LLMClient client(make_config(server.base_url(), scratch.path));
        client.initialize();

        const AgentState final = client.run_one_turn("hello");

        // The breaker must end the turn — pre-fix this looped to the
        // max-iterations cap.
        CHECK(final == AgentState::DONE);

        // Exactly 2 requests: first status-only iteration counts, second
        // trips the breaker. Pre-fix: one request per iteration up to cap.
        CHECK(server.request_count() == 2);
    }

    TEST_CASE("one set_status call then a text reply does NOT trip the breaker") {
        StatusOnlyServer server(StatusOnlyServer::STATUS_THEN_TEXT);
        ScratchDir scratch("status-then-text");

        LLMClient client(make_config(server.base_url(), scratch.path));
        client.initialize();

        const AgentState final = client.run_one_turn("hello");

        // Turn ends via the normal text-response path on iteration 2 —
        // a single status-only iteration is a legitimate UI update.
        CHECK(final == AgentState::DONE);
        CHECK(server.request_count() == 2);
    }

    TEST_CASE("breaker resets between turns") {
        StatusOnlyServer server(StatusOnlyServer::STATUS_THEN_TEXT);
        ScratchDir scratch("status-reset");

        LLMClient client(make_config(server.base_url(), scratch.path));
        client.initialize();

        // Turn 1: status-only (count 1) then text (reset) — ends normally.
        CHECK(client.run_one_turn("first") == AgentState::DONE);
        const int after_first = server.request_count();
        CHECK(after_first == 2);

        // Turn 2: same pattern. If the counter had leaked across turns,
        // turn 2's first status-only iteration would trip the breaker
        // immediately — same observable result (DONE after 2 requests),
        // so assert the count instead of just the state.
        CHECK(client.run_one_turn("second") == AgentState::DONE);
        CHECK(server.request_count() == 4);
    }
}
