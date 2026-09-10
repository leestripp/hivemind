// tests/test_model_discovery.cpp
//
// Unit tests for hive::discover_models().
// Uses a tiny in-process HTTP server bound to 127.0.0.1:0 so the tests are
// hermetic and require no external API keys.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "library/model_discovery.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

class LocalServer {
public:
    explicit LocalServer(const std::string& response_body,
                         long status_code = 200,
                         const std::string& content_type = "application/json")
        : body_(response_body),
          status_code_(status_code),
          content_type_(content_type),
          running_(false) {
        listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (listen_fd_ < 0) throw std::runtime_error("socket() failed");
        int yes = 1;
        ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;
        if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0)
            throw std::runtime_error("bind failed");
        if (::listen(listen_fd_, 1) < 0) throw std::runtime_error("listen failed");

        socklen_t len = sizeof(addr);
        ::getsockname(listen_fd_, reinterpret_cast<sockaddr*>(&addr), &len);
        port_ = ntohs(addr.sin_port);

        running_.store(true);
        thread_ = std::thread([this] { loop(); });
    }

    ~LocalServer() {
        running_.store(false);
        ::shutdown(listen_fd_, SHUT_RDWR);
        ::close(listen_fd_);
        if (thread_.joinable()) thread_.join();
    }

    [[nodiscard]] int port() const { return port_; }
    [[nodiscard]] std::string url() const { return "http://127.0.0.1:" + std::to_string(port_) + "/"; }

    std::string last_request;
    std::mutex request_mu;

private:
    void loop() {
        while (running_.load()) {
            sockaddr_in caddr{};
            socklen_t clen = sizeof(caddr);
            int cfd = ::accept(listen_fd_, reinterpret_cast<sockaddr*>(&caddr), &clen);
            if (cfd < 0) break;
            handle_one(cfd);
            ::close(cfd);
        }
    }

    void handle_one(int cfd) {
        std::string in;
        char buf[4096];
        while (true) {
            ssize_t n = ::recv(cfd, buf, sizeof(buf), 0);
            if (n <= 0) break;
            in.append(buf, static_cast<std::size_t>(n));
            if (in.find("\r\n\r\n") != std::string::npos) break;
            if (in.size() > 64 * 1024) break;
        }
        {
            std::lock_guard<std::mutex> lk(request_mu);
            last_request = in;
        }

        const std::string status_line = "HTTP/1.1 " + std::to_string(status_code_) + " OK\r\n";
        std::string resp = status_line +
                           "Content-Type: " + content_type_ + "\r\n" +
                           "Content-Length: " + std::to_string(body_.size()) + "\r\n" +
                           "Connection: close\r\n\r\n" + body_;
        ::send(cfd, resp.data(), resp.size(), 0);
    }

    std::string body_;
    long status_code_;
    std::string content_type_;
    std::atomic<bool> running_;
    int listen_fd_ = -1;
    int port_ = 0;
    std::thread thread_;
};

} // anonymous namespace

TEST_CASE("discover_models parses a standard OpenAI-compatible response") {
    LocalServer srv(R"({
        "object": "list",
        "data": [
            {"id": "MiniMax-M3", "object": "model", "created": 1780272000, "owned_by": "minimax"},
            {"id": "MiniMax-M2.7", "object": "model", "created": 1773799200, "owned_by": "minimax"}
        ]
    })");

    auto result = hive::discover_models(srv.url(), "test-key");

    REQUIRE(result.success);
    REQUIRE(result.error_message.empty());
    REQUIRE(result.models.size() == 2);
    CHECK(result.models[0].id == "MiniMax-M3");
    CHECK(result.models[0].owned_by == "minimax");
    CHECK(result.models[0].created == 1780272000);
    CHECK(result.models[1].id == "MiniMax-M2.7");

    {
        std::lock_guard<std::mutex> lk(srv.request_mu);
        CHECK(srv.last_request.find("GET /v1/models HTTP/1.1") != std::string::npos);
        // Header name may be lower-cased by the HTTP client; just verify the
        // bearer token is present somewhere in the request headers.
        CHECK(srv.last_request.find("Bearer test-key") != std::string::npos);
    }
}

TEST_CASE("discover_models handles an empty model list") {
    LocalServer srv(R"({"object": "list", "data": []})");

    auto result = hive::discover_models(srv.url(), "test-key");

    REQUIRE(result.success);
    CHECK(result.models.empty());
}

TEST_CASE("discover_models strips trailing slashes from host") {
    LocalServer srv(R"({"object": "list", "data": []})");

    auto result = hive::discover_models(srv.url() + "////", "test-key");

    REQUIRE(result.success);
    {
        std::lock_guard<std::mutex> lk(srv.request_mu);
        CHECK(srv.last_request.find("GET /v1/models HTTP/1.1") != std::string::npos);
        CHECK(srv.last_request.find("GET //v1/models") == std::string::npos);
    }
}

TEST_CASE("discover_models avoids double /v1 when host includes it") {
    LocalServer srv(R"({"object": "list", "data": []})");

    auto result = hive::discover_models(srv.url() + "v1", "test-key", "/v1/models");

    REQUIRE(result.success);
    {
        std::lock_guard<std::mutex> lk(srv.request_mu);
        CHECK(srv.last_request.find("GET /v1/models HTTP/1.1") != std::string::npos);
        CHECK(srv.last_request.find("GET /v1/v1/models") == std::string::npos);
    }
}

TEST_CASE("discover_models reports error when data array is missing") {
    LocalServer srv(R"({"object": "list"})");

    auto result = hive::discover_models(srv.url(), "test-key");

    CHECK_FALSE(result.success);
    CHECK(result.error_message.find("missing or invalid 'data'") != std::string::npos);
}

TEST_CASE("discover_models reports error on non-list object field") {
    LocalServer srv(R"({"object": "foo", "data": []})");

    auto result = hive::discover_models(srv.url(), "test-key");

    CHECK_FALSE(result.success);
    CHECK(result.error_message.find("object is not 'list'") != std::string::npos);
}

TEST_CASE("discover_models reports error on malformed JSON") {
    LocalServer srv("not json");

    auto result = hive::discover_models(srv.url(), "test-key");

    CHECK_FALSE(result.success);
    CHECK(result.error_message.find("JSON parse error") != std::string::npos);
}

TEST_CASE("discover_models reports error on HTTP failure") {
    LocalServer srv("{}", 500);

    auto result = hive::discover_models(srv.url(), "test-key");

    CHECK_FALSE(result.success);
    CHECK(result.error_message.find("HTTP 500") != std::string::npos);
}

TEST_CASE("discover_models reports error when host is empty") {
    auto result = hive::discover_models("", "test-key");

    CHECK_FALSE(result.success);
    CHECK(result.error_message.find("host is empty") != std::string::npos);
}

TEST_CASE("discover_models skips malformed entries without id") {
    LocalServer srv(R"({
        "object": "list",
        "data": [
            {"object": "model", "owned_by": "minimax"},
            {"id": "good-model", "object": "model", "owned_by": "minimax"}
        ]
    })");

    auto result = hive::discover_models(srv.url(), "test-key");

    REQUIRE(result.success);
    REQUIRE(result.models.size() == 1);
    CHECK(result.models[0].id == "good-model");
}

TEST_CASE("discover_models parses extended capability fields") {
    LocalServer srv(R"({
        "object": "list",
        "data": [
            {
                "id": "kimi-k2.5",
                "object": "model",
                "created": 1780272000,
                "owned_by": "moonshot",
                "context_length": 256000,
                "supports_image_in": true,
                "supports_video_in": false,
                "supports_reasoning": true
            },
            {
                "id": "gpt-4o",
                "object": "model",
                "created": 1780272001,
                "owned_by": "openai"
            }
        ]
    })");

    auto result = hive::discover_models(srv.url(), "test-key");

    REQUIRE(result.success);
    REQUIRE(result.models.size() == 2);
    CHECK(result.models[0].context_length == 256000);
    CHECK(result.models[0].supports_vision == true);
    CHECK(result.models[0].supports_reasoning == true);
    CHECK(result.models[1].context_length == 0);
    CHECK(result.models[1].supports_vision == false);
    CHECK(result.models[1].supports_reasoning == false);
}
