/**
 * @file test_fatal_breaker.cpp
 * @brief Regression tests for the fatal-error infinite-retry bug reported
 *        by the MonkeyByte host (daedalus_bugs.md §2).
 *
 * When a provider rejects every request with a terminal 4xx whose error
 * shape ALTERNATES between cycles (each injected recovery message corrupts
 * the history into the next shape — e.g. "function name X is duplicated"
 * then "messages with role 'tool' must be a response..."), the original
 * same-signature circuit breaker never tripped and the agent looped for
 * hundreds of iterations, appending one [SYSTEM ERROR] user message per
 * cycle.
 *
 * The fix counts ANY consecutive fatal-classified cycle (threshold 2) and
 * ends the turn WITHOUT further injection. These tests drive LLMClient
 * against an in-process HTTP server that alternates its 400 error payload.
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

// ---------------------------------------------------------------------------
// In-process HTTP server that answers every POST with 400 + a structured
// provider error body, ALTERNATING the message per request (the exact
// failure shape that defeated the same-signature breaker).
// ---------------------------------------------------------------------------
class Alternating400Server {
public:
    Alternating400Server() {
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

    ~Alternating400Server() {
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
        // Alternate between the two shapes seen in the incident.
        const std::string message = (seq % 2 == 0)
            ? "function name app_ui_control is duplicated"
            : "messages with role 'tool' must be a response to a preceding message with 'tool_calls'";
        const std::string body =
            R"({"error":{"type":"invalid_request_error","message":")" + message + "\"}}";

        const std::string resp =
            "HTTP/1.1 400 Bad Request\r\n"
            "Content-Type: application/json\r\n"
            "Content-Length: " + std::to_string(body.size()) + "\r\n"
            "Connection: close\r\n\r\n" + body;
        ::send(cfd, resp.data(), resp.size(), 0);
    }

    int listen_fd_ = -1;
    int port_ = 0;
    std::atomic<bool> running_{false};
    std::atomic<int> request_count_{0};
    std::thread thread_;
};

// ---------------------------------------------------------------------------
struct CapturingSink : hive::AgentSink {
    struct ErrorEvent {
        hive::ErrorKind kind;
        std::string message;
        bool will_retry;
    };
    std::mutex mu;
    std::vector<ErrorEvent> errors;

    void on_error(hive::ErrorKind kind, std::string_view message,
                  bool will_retry) noexcept override {
        std::lock_guard<std::mutex> lk(mu);
        errors.push_back({kind, std::string(message), will_retry});
    }
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

} // namespace

TEST_SUITE("fatal API error circuit breaker") {

    TEST_CASE("alternating fatal 400 shapes end the turn after 2 cycles") {
        Alternating400Server server;
        ScratchDir scratch("fatal-breaker");

        AppConfig cfg;
        cfg.base_url = server.base_url();
        cfg.chat_endpoint = "/v1/chat/completions";
        cfg.api_key = "test-key";
        cfg.model = "test-model";
        cfg.plugin = "openai-compatible";  // loopback hosts auto-detect lmstudio
        cfg.target_directory = scratch.path;
        cfg.memory_disabled = true;
        cfg.stream = false;  // buffered JSON path — no SSE needed from the fake

        LLMClient client(cfg);
        auto sink = std::make_shared<CapturingSink>();
        client.set_sink(sink);
        client.initialize();

        const AgentState final = client.run_one_turn("hello");

        // The breaker must end the turn — pre-fix this looped to the
        // 100-iteration cap.
        CHECK(final == AgentState::DONE);

        // Exactly 2 requests: one recovery attempt, then the breaker trips.
        // Pre-fix (alternating signatures) this was 100 requests.
        CHECK(server.request_count() == 2);

        // History stays bounded: system + per-cycle context-status + user +
        // exactly ONE injected [SYSTEM ERROR] recovery message. Pre-fix the
        // loop appended one recovery message per cycle (~100 by the cap).
        CHECK(client.get_message_count() <= 4);

        // The host saw a terminal (will_retry=false) error.
        bool saw_terminal = false;
        {
            std::lock_guard<std::mutex> lk(sink->mu);
            for (const auto& e : sink->errors) {
                if (!e.will_retry) saw_terminal = true;
            }
        }
        CHECK(saw_terminal);
    }
}
