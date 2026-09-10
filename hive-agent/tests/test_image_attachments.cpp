/**
 * @file test_image_attachments.cpp
 * @brief Regression tests for the host image-attachment bug (0.10.0):
 *        `![alt](path)` markdown and Agent::submit_user_message(text, imgs)
 *        never reached the model (build_multimodal_message() was dead code).
 *
 * The fix routes host attachments through the read_image pipeline:
 * queued at message time, injected into the NEXT request payload only
 * (stored history keeps plain Markdown text), gated on
 * model_supports_vision(). These tests capture the outgoing payload with
 * an in-process HTTP server (pattern from test_fatal_breaker.cpp).
 */

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <atomic>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include "api/ImageRefs.h"
#include "api/LLMClient.h"
#include "core/FileIO.h"
#include "library/AgentSink.h"

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {

// 1x1 transparent PNG.
const char* kTinyPngB64 =
    "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mNk"
    "YPhfDwAChwGA60e6kgAAAABJRU5ErkJggg==";

std::vector<unsigned char> base64_decode(const std::string& in) {
    static const std::string chars =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::vector<unsigned char> out;
    int val = 0, bits = -8;
    for (unsigned char c : in) {
        if (c == '=') break;
        auto pos = chars.find(static_cast<char>(c));
        if (pos == std::string::npos) continue;
        val = (val << 6) + static_cast<int>(pos);
        bits += 6;
        if (bits >= 0) {
            out.push_back(static_cast<unsigned char>((val >> bits) & 0xFF));
            bits -= 8;
        }
    }
    return out;
}

struct ScratchDir {
    fs::path path;
    explicit ScratchDir(const std::string& tag) {
        path = fs::temp_directory_path() /
               ("hive_agent_test_" + tag + "_" +
                std::to_string(reinterpret_cast<std::uintptr_t>(this)));
        fs::remove_all(path);
        fs::create_directories(path);
    }
    ~ScratchDir() { std::error_code ec; fs::remove_all(path, ec); }
};

// Captures the LAST request body and replies with a minimal valid
// chat completion (assistant text, no tool calls -> turn ends DONE).
class CaptureServer {
public:
    CaptureServer() {
        listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        int yes = 1;
        ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;
        REQUIRE(::bind(listen_fd_, (sockaddr*)&addr, sizeof(addr)) == 0);
        REQUIRE(::listen(listen_fd_, 4) == 0);
        socklen_t len = sizeof(addr);
        ::getsockname(listen_fd_, (sockaddr*)&addr, &len);
        port_ = ntohs(addr.sin_port);
        running_.store(true);
        thread_ = std::thread([this] { loop(); });
    }
    ~CaptureServer() {
        running_.store(false);
        ::shutdown(listen_fd_, SHUT_RDWR);
        ::close(listen_fd_);
        if (thread_.joinable()) thread_.join();
    }
    std::string base_url() const { return "http://127.0.0.1:" + std::to_string(port_); }
    std::string last_body() const { std::lock_guard<std::mutex> lk(mu_); return last_body_; }

private:
    void loop() {
        while (running_.load()) {
            int fd = ::accept(listen_fd_, nullptr, nullptr);
            if (fd < 0) break;
            std::string req;
            char buf[65536];
            // Read headers
            size_t header_end = std::string::npos;
            while (header_end == std::string::npos) {
                ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
                if (n <= 0) break;
                req.append(buf, static_cast<size_t>(n));
                header_end = req.find("\r\n\r\n");
            }
            size_t content_length = 0;
            {
                auto pos = req.find("Content-Length:");
                if (pos != std::string::npos)
                    content_length = static_cast<size_t>(std::stoul(req.substr(pos + 15)));
            }
            std::string body = header_end != std::string::npos
                ? req.substr(header_end + 4) : "";
            while (body.size() < content_length) {
                ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
                if (n <= 0) break;
                body.append(buf, static_cast<size_t>(n));
            }
            { std::lock_guard<std::mutex> lk(mu_); last_body_ = body; }

            static const char* kResp =
                "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
                "Content-Length: 156\r\nConnection: close\r\n\r\n"
                "{\"id\":\"x\",\"choices\":[{\"index\":0,\"message\":{\"role\":\"assistant\","
                "\"content\":\"ok\"},\"finish_reason\":\"stop\"}],"
                "\"usage\":{\"prompt_tokens\":1,\"completion_tokens\":1,\"total_tokens\":2}}";
            ::send(fd, kResp, std::strlen(kResp), MSG_NOSIGNAL);
            ::close(fd);
        }
    }
    int listen_fd_ = -1;
    int port_ = 0;
    std::atomic<bool> running_{false};
    std::thread thread_;
    mutable std::mutex mu_;
    std::string last_body_;
};

// Default AgentSink no-ops are fine — assertions read the captured
// request payload, not sink events.
struct QuietSink : hive::AgentSink {};

} // namespace

TEST_SUITE("image_refs extraction") {
    TEST_CASE("extracts refs in order") {
        auto refs = hive::image_refs::extract(
            "see ![a](one.png) and ![b](https://x/y.jpg) done");
        REQUIRE(refs.size() == 2);
        CHECK(refs[0] == "one.png");
        CHECK(refs[1] == "https://x/y.jpg");
    }
    TEST_CASE("no refs / malformed ignored") {
        CHECK(hive::image_refs::extract("plain text").empty());
        CHECK(hive::image_refs::extract("![unclosed").empty());
        CHECK(hive::image_refs::extract("![a](x.png").empty());
        CHECK(hive::image_refs::extract("[link](not-image.md)").empty());
    }
}

TEST_SUITE("host image attachments reach the payload") {

TEST_CASE("![alt](path) markdown attaches image_url part, history stays text") {
    ScratchDir scratch("img_md");
    {
        std::ofstream f(scratch.path / "dot.png", std::ios::binary);
        auto bytes = base64_decode(kTinyPngB64);
        f.write(reinterpret_cast<const char*>(bytes.data()),
                static_cast<std::streamsize>(bytes.size()));
    }
    CaptureServer server;

    AppConfig cfg;
    cfg.base_url = server.base_url();
    cfg.chat_endpoint = "/v1/chat/completions";
    cfg.api_key = "test-key";
    cfg.model = "custom-vision-model";   // not in any table -> vision assumed
    cfg.plugin = "openai-compatible";    // loopback hosts auto-detect lmstudio
    cfg.target_directory = scratch.path;
    cfg.memory_disabled = true;
    cfg.stream = false;

    LLMClient client(cfg);
    client.set_sink(std::make_shared<QuietSink>());
    client.initialize();

    const AgentState final = client.run_one_turn("what is ![dot](dot.png)?");
    CHECK(final == AgentState::DONE);

    const std::string body = server.last_body();
    REQUIRE(!body.empty());
    json payload = json::parse(body);
    REQUIRE(payload.contains("messages"));

    bool found_image_part = false;
    bool stored_user_msg_is_plain_text = false;
    for (const auto& m : payload["messages"]) {
        if (m.value("role", "") == "user" && m["content"].is_array()) {
            for (const auto& part : m["content"]) {
                if (part.value("type", "") == "image_url" &&
                    part["image_url"].value("url", "").rfind("data:image/png;base64,", 0) == 0) {
                    found_image_part = true;
                }
                if (part.value("type", "") == "text") {
                    CHECK(part.value("text", "").find("user attachment") != std::string::npos);
                }
            }
        }
        // The stored history message keeps the cheap Markdown text.
        if (m.value("role", "") == "user" && m["content"].is_string() &&
            m["content"].get<std::string>().find("![dot](dot.png)") != std::string::npos) {
            stored_user_msg_is_plain_text = true;
        }
    }
    CHECK(found_image_part);
    CHECK(stored_user_msg_is_plain_text);
}

TEST_CASE("text-only model drops the image instead of risking a 400") {
    ScratchDir scratch("img_gate");
    {
        std::ofstream f(scratch.path / "dot.png", std::ios::binary);
        auto bytes = base64_decode(kTinyPngB64);
        f.write(reinterpret_cast<const char*>(bytes.data()),
                static_cast<std::streamsize>(bytes.size()));
    }
    CaptureServer server;

    AppConfig cfg;
    cfg.base_url = server.base_url();
    cfg.chat_endpoint = "/v1/chat/completions";
    cfg.api_key = "test-key";
    cfg.model = "MiniMax-M2";       // in MiniMax table: supports_vision=false
    cfg.plugin = "minimax";         // explicit plugin wins over host autodetect
    cfg.target_directory = scratch.path;
    cfg.memory_disabled = true;
    cfg.stream = false;

    LLMClient client(cfg);
    client.set_sink(std::make_shared<QuietSink>());
    client.initialize();

    const AgentState final = client.run_one_turn("look ![dot](dot.png)");
    CHECK(final == AgentState::DONE);

    const std::string body = server.last_body();
    REQUIRE(!body.empty());
    CHECK(body.find("image_url") == std::string::npos);
    CHECK(body.find("data:image/png;base64,") == std::string::npos);
}

} // TEST_SUITE
