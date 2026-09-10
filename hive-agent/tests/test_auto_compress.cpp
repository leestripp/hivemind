// SPDX-License-Identifier: BSL-1.0
// tests/test_auto_compress.cpp
//
// §AutoCompress (2026-08-01): end-to-end proof that the context-compression
// loop in LLMClient actually fires and is observable through the PUBLIC
// API — the gap left by the unit tests, which only cover
// Messages::should_compress() thresholds and Messages::compress() mechanics
// in isolation.
//
// Drives a full hive::Agent against an in-process HTTP server:
//   - The model row's max_context_size is tiny (10000), and the fake
//     server's first chat response reports usage.total_tokens = 9500,
//     tripping the 90% auto-compress threshold after cycle 1.
//   - The compression summary request (distinguishable by the
//     "max_tokens" field Messages::call_llm_for_summary adds) gets a
//     summary completion back.
//   - The second cycle reports low usage, so the loop ends DONE.
//
// Asserts the host-visible contract: AgentSink::on_compaction(before,
// after) fired exactly once with after < before, the fake server saw the
// compression request, and Agent::stats() reflects the post-compression
// turn.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include "library/Agent.h"
#include "api/ProviderPlugin.h"
#include "api/ProviderRegistry.h"
#include "library/model_discovery.h"
#include "a2a/A2aClient.h"
#include "core/settings.h"

// §COMPRESS-TOOL: unit suite for LLMClient::handle_compress_context_result
// lives at the bottom of this file.
#include "api/LLMClient.h"
#include <nlohmann/json.hpp>

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {

// ---------------------------------------------------------------------------
// In-process HTTP server. Answers POSTs with chat-completion JSON:
//   - body contains "max_tokens"  → compression summary request
//   - otherwise                   → main agent turn (high usage on the first,
//                                   low usage afterwards)
// ---------------------------------------------------------------------------
class CompressionFakeServer {
public:
    // first_total/later_total: usage.total_tokens reported for the first
    // main-turn response and all later ones. Defaults trip the 90%
    // threshold of a 10000-token context on cycle 1.
    // fail_compression: answer every compression summary request with
    // HTTP 500 — drives Messages::compress() down its last-resort
    // fallback-delete path (§COMPRESS-REPORT).
    explicit CompressionFakeServer(int first_total = 9500, int later_total = 100,
                                   bool fail_compression = false,
                                   bool emit_compress_tool_call = false)
        : first_total_(first_total), later_total_(later_total),
          fail_compression_(fail_compression),
          emit_compress_tool_call_(emit_compress_tool_call) {
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

    ~CompressionFakeServer() {
        running_.store(false);
        ::shutdown(listen_fd_, SHUT_RDWR);
        ::close(listen_fd_);
        if (thread_.joinable()) thread_.join();
    }

    std::string base_url() const {
        return "http://127.0.0.1:" + std::to_string(port_);
    }
    int main_requests() const        { return main_requests_.load(); }
    int compression_requests() const { return compression_requests_.load(); }
    // §BUG-COMPRESS-TEMP: raw body of the most recent compression summary
    // request — lets tests assert the payload carries the clamped
    // temperature the selected model requires.
    std::string last_compression_body() const {
        std::lock_guard<std::mutex> lk(body_mu_);
        return last_compression_body_;
    }

private:
    static void send_json(int cfd, const std::string& body) {
        const std::string resp =
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: application/json\r\n"
            "Content-Length: " + std::to_string(body.size()) + "\r\n"
            "Connection: close\r\n\r\n" + body;
        ::send(cfd, resp.data(), resp.size(), 0);
    }

    static std::string chat_body(const std::string& content, int total_tokens) {
        json j = {
            {"id", "cmpl-fake"},
            {"object", "chat.completion"},
            {"choices", json::array({{
                {"index", 0},
                {"message", {{"role", "assistant"}, {"content", content}}},
                {"finish_reason", "stop"},
            }})},
            {"usage", {{"prompt_tokens", total_tokens - 10},
                       {"completion_tokens", 10},
                       {"total_tokens", total_tokens}}},
        };
        return j.dump();
    }

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
        const std::string body = in.substr(body_start + 4);

        if (body.find("\"max_tokens\"") != std::string::npos) {
            // Messages::call_llm_for_summary — the compression request.
            compression_requests_.fetch_add(1);
            {
                std::lock_guard<std::mutex> lk(body_mu_);
                last_compression_body_ = body;
            }
            if (fail_compression_) {
                // 5xx is retried (fast — no backoff), so all 3 attempts
                // land here and the summary call gives up → fallback.
                const std::string resp =
                    "HTTP/1.1 500 Internal Server Error\r\n"
                    "Content-Length: 0\r\n"
                    "Connection: close\r\n\r\n";
                ::send(cfd, resp.data(), resp.size(), 0);
                return;
            }
            send_json(cfd, chat_body("Summary: the user greeted the agent.", 520));
            return;
        }

        // Main agent turn: configured usage for cycle 1 vs. later cycles.
        const int seq = main_requests_.fetch_add(1);
        if (seq == 0 && emit_compress_tool_call_) {
            // §COMPRESS-TOOL: the model asks for compression itself via the
            // compress_context tool on cycle 1.
            send_json(cfd, tool_call_body(first_total_));
        } else if (seq == 0) {
            send_json(cfd, chat_body("Hi there!", first_total_));
        } else {
            send_json(cfd, chat_body("All done.", later_total_));
        }
    }

    // Assistant message carrying a compress_context tool call.
    static std::string tool_call_body(int total_tokens) {
        json j = {
            {"id", "cmpl-fake"},
            {"object", "chat.completion"},
            {"choices", json::array({{
                {"index", 0},
                {"message", {
                    {"role", "assistant"},
                    {"content", nullptr},
                    {"tool_calls", json::array({{
                        {"id", "call_compress_1"},
                        {"type", "function"},
                        {"function", {
                            {"name", "compress_context"},
                            {"arguments", "{\"reason\": \"research complete, shed early context\"}"}
                        }},
                    }})},
                }},
                {"finish_reason", "tool_calls"},
            }})},
            {"usage", {{"prompt_tokens", total_tokens - 10},
                       {"completion_tokens", 10},
                       {"total_tokens", total_tokens}}},
        };
        return j.dump();
    }

    int listen_fd_ = -1;
    int port_ = 0;
    int first_total_ = 9500;
    int later_total_ = 100;
    bool fail_compression_ = false;
    bool emit_compress_tool_call_ = false;
    std::atomic<bool> running_{false};
    std::atomic<int> main_requests_{0};
    std::atomic<int> compression_requests_{0};
    mutable std::mutex body_mu_;
    std::string last_compression_body_;
    std::thread thread_;
};

// ---------------------------------------------------------------------------
struct CompactionSink : hive::AgentSink {
    struct Event { std::size_t before, after; };
    std::mutex mu;
    std::vector<Event> compactions;
    std::vector<std::string> errors;

    void on_compaction(std::size_t before, std::size_t after) noexcept override {
        std::lock_guard<std::mutex> lk(mu);
        compactions.push_back({before, after});
    }
    void on_error(hive::ErrorKind, std::string_view message,
                  bool) noexcept override {
        std::lock_guard<std::mutex> lk(mu);
        errors.emplace_back(message);
    }
};

fs::path make_temp_home(const std::string& tag) {
    fs::path p = fs::temp_directory_path() /
        ("hive_agent_test_auto_compress_" + tag + "_" +
         std::to_string(
             std::chrono::system_clock::now().time_since_epoch().count()));
    fs::remove_all(p);
    fs::create_directories(p);
    return p;
}

// §BUG-MEMORY-BLOAT: the run loop skips auto-compress for trivially small
// histories (< 10 non-system messages) — compression can only rewrite
// non-system messages, so when the history is a handful of entries the
// pressure is by definition outside its reach and it would shred the
// history for zero gain, re-triggering every cycle. The auto-trigger
// tests below therefore seed a realistic restored-session history on
// disk; the agent loads it like any returning session.
void seed_history(const fs::path& project, int non_system_messages) {
    const fs::path dir = project / ".hive-agent" / "default";
    fs::create_directories(dir);
    json history = json::array();
    history.push_back({{"role", "system"}, {"content", "test system prompt"}});
    for (int i = 0; i < non_system_messages; ++i) {
        history.push_back(
            {{"role", i % 2 == 0 ? "user" : "assistant"},
             {"content", "seeded message " + std::to_string(i) +
                         " — filler text to give the restored history body."}});
    }
    std::ofstream f(dir / "messages.json");
    f << history.dump(2);
}

} // namespace

// ---------------------------------------------------------------------------
// §SYNC-FALLBACK-STREAM (2026-08-01): fake server for the sync-fallback
// regression test. Request 1 gets HTTP 200 with a structured TRANSIENT
// error body (internal_error) — this bypasses the request layer's own
// retry (2xx returns immediately) and drops the agent cycle into its
// "Transient error - retry with backoff" loop, which re-posts via the
// non-streaming sync fallback. Any later request that still carries
// "stream":true means the fallback forgot to force stream off — the old
// bug, where the server answered with SSE and json::parse failed on
// "data: ..." ('d'). (A 429 would not work here: the request layer
// retries those itself, legitimately, via the SSE-capable path.)
// ---------------------------------------------------------------------------
namespace {

class TransientErrorServer {
public:
    TransientErrorServer() {
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

    ~TransientErrorServer() {
        running_.store(false);
        ::shutdown(listen_fd_, SHUT_RDWR);
        ::close(listen_fd_);
        if (thread_.joinable()) thread_.join();
    }

    std::string base_url() const {
        return "http://127.0.0.1:" + std::to_string(port_);
    }
    int requests() const              { return requests_.load(); }
    int stream_violations() const     { return stream_violations_.load(); }

private:
    void send_raw(int cfd, int code, const std::string& status,
                  const std::string& body, const std::string& content_type) {
        const std::string resp =
            "HTTP/1.1 " + std::to_string(code) + " " + status + "\r\n"
            "Content-Type: " + content_type + "\r\n"
            "Content-Length: " + std::to_string(body.size()) + "\r\n"
            "Connection: close\r\n\r\n" + body;
        ::send(cfd, resp.data(), resp.size(), 0);
    }

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
        const std::string body = in.substr(body_start + 4);

        const int seq = requests_.fetch_add(1);
        if (seq == 0) {
            // HTTP 200 + structured TRANSIENT error → the agent cycle's
            // "retry with backoff" loop re-posts via the sync fallback.
            send_raw(cfd, 200, "OK",
                     R"({"error":{"type":"internal_error","message":"temporary blip"}})",
                     "application/json");
            return;
        }

        // The retry must NOT still ask for SSE — nlohmann dumps the flag
        // as "stream":true (no space).
        if (body.find("\"stream\":true") != std::string::npos) {
            stream_violations_.fetch_add(1);
            // Mimic the old failure: an SSE body that json::parse chokes on.
            send_raw(cfd, 200, "OK",
                     "data: {\"choices\":[{\"delta\":{\"content\":\"x\"}}]}\n\n",
                     "text/event-stream");
            return;
        }
        send_raw(cfd, 200, "OK",
                 R"({"id":"cmpl-fake","choices":[{"index":0,"message":)"
                 R"({"role":"assistant","content":"recovered"},"finish_reason":"stop"}],)"
                 R"("usage":{"prompt_tokens":10,"completion_tokens":5,"total_tokens":15}})",
                 "application/json");
    }

    int listen_fd_ = -1;
    int port_ = 0;
    std::atomic<bool> running_{false};
    std::atomic<int> requests_{0};
    std::atomic<int> stream_violations_{0};
    std::thread thread_;
};

} // namespace

TEST_CASE("streaming retry fallback forces stream off (no SSE parse crash)") {
    TransientErrorServer server;
    fs::path home = make_temp_home("sync-fallback");

    {
        auto store = hive::SettingsStore::for_home(home);
        hive::ProviderConfig p;
        p.name          = "fake";
        p.host          = server.base_url();
        p.api_key       = "test-key";
        p.chat_endpoint = "/v1/chat/completions";
        p.plugin        = "openai-compatible";
        store.upsert_provider(p);

        hive::ModelConfig m;
        m.name             = "tiny";
        m.provider         = "fake";
        m.model_id         = "fake-model";
        m.max_context_size = 100000;
        m.temperature      = 0.0;
        store.upsert_model(m);
        store.set_default_model_name("tiny");
    }

    fs::path project = home / "project";
    fs::create_directories(project);

    hive::AgentConfig cfg;
    cfg.project_root   = project;
    cfg.agent_home  = home;
    cfg.session_id     = "sync-fallback-e2e";
    cfg.stream         = true;    // the bug only bites in streaming mode
    cfg.memory_enabled = false;
    cfg.mcp_enabled    = false;

    auto sink = std::make_shared<CompactionSink>();
    hive::Agent agent(std::move(cfg), sink);
    agent.initialize();

    agent.submit_user_message("hello");
    agent.run_one_turn();
    agent.shutdown();

    // The retry recovered through the sync fallback: no request after the
    // structured-error 200 still carried "stream":true, and the turn
    // completed without surfacing a fatal error.
    CHECK(server.stream_violations() == 0);
    CHECK(server.requests() >= 2);
    {
        std::lock_guard<std::mutex> lk(sink->mu);
        for (const auto& e : sink->errors) {
            CHECK(e.find("parse error") == std::string::npos);
        }
    }

    fs::remove_all(home);
}

TEST_CASE("auto-compress at 90% fires AgentSink::on_compaction end-to-end") {
    CompressionFakeServer server;
    fs::path home = make_temp_home("e2e");

    // Seed the store: provider pointing at the fake server, one model with
    // a tiny 10000-token context so 9500 reported tokens trips the 90%
    // threshold. plugin pinned to openai-compatible (loopback hosts
    // auto-detect as lmstudio).
    {
        auto store = hive::SettingsStore::for_home(home);
        hive::ProviderConfig p;
        p.name          = "fake";
        p.host          = server.base_url();
        p.api_key       = "test-key";
        p.chat_endpoint = "/v1/chat/completions";
        p.plugin        = "openai-compatible";
        store.upsert_provider(p);

        hive::ModelConfig m;
        m.name             = "tiny";
        m.provider         = "fake";
        m.model_id         = "fake-model";
        m.max_context_size = 10000;
        m.temperature      = 0.0;
        store.upsert_model(m);
        store.set_default_model_name("tiny");
    }

    fs::path project = home / "project";
    fs::create_directories(project);

    hive::AgentConfig cfg;
    cfg.project_root   = project;
    cfg.agent_home  = home;
    cfg.session_id     = "auto-compress-e2e";
    cfg.stream         = false;   // buffered JSON keeps the fake server simple
    // memory_enabled must stay TRUE here: !memory_enabled makes
    // initialize() WIPE messages.json before Messages::load() runs
    // (AgentImpl "no prior context of any kind"), which would delete the
    // seeded history this test relies on.
    cfg.memory_enabled = true;
    cfg.mcp_enabled    = false;

    // §BUG-MEMORY-BLOAT: needs >= 10 non-system messages or the
    // small-history guard skips auto-compress.
    seed_history(project, 12);

    auto sink = std::make_shared<CompactionSink>();
    hive::Agent agent(std::move(cfg), sink);
    agent.initialize();

    agent.submit_user_message("hello");
    agent.run_one_turn();

    // The host-visible compression event fired exactly once, and the
    // post-compression estimate is smaller than the pre-compression count.
    {
        std::lock_guard<std::mutex> lk(sink->mu);
        REQUIRE(sink->compactions.size() == 1);
        CHECK(sink->compactions[0].before == 9500);
        CHECK(sink->compactions[0].after < sink->compactions[0].before);
        CHECK(sink->errors.empty());
    }

    // The server saw: cycle 1 (high usage) → compression summary request →
    // cycle 2 (low usage, DONE).
    CHECK(server.compression_requests() == 1);
    CHECK(server.main_requests() == 2);

    // Post-compression the loop ran one more cycle whose low usage is what
    // AgentStats now reports — proof the counters reset through compression
    // instead of accumulating the discarded 9500.
    CHECK(agent.stats().total_tokens == 100);

    // §COMPRESS-USER-MSG: the first message after system in the compressed
    // history must be a user message — strict chat templates (qwen3 jinja
    // on LM Studio) reject assistant/tool-first histories with "No user
    // query found in messages". Checked after shutdown() — the flush
    // worker does its final save on destruction, so the transcript isn't
    // on disk mid-run.
    agent.shutdown();

    {
        const fs::path transcript =
            project / ".hive-agent" / "default" / "messages.json";
        REQUIRE(fs::is_regular_file(transcript));
        std::ifstream f(transcript);
        const json history = json::parse(f);
        REQUIRE(history.size() >= 2);
        CHECK(history[0].value("role", "") == "system");
        CHECK(history[1].value("role", "") == "user");
    }

    fs::remove_all(home);
}


TEST_CASE("compression request carries provider-clamped temperature") {
    // §BUG-COMPRESS-TEMP (2026-08-08) — DO NOT DELETE THIS TEST.
    // Messages::call_llm_for_summary posts its own hand-built payload and
    // never passes through LLMClient::run_agent_cycle(), so the provider
    // plugin's temperature clamp was skipped: the compression request went
    // out with the model row's raw temperature (0.0). Fixed-temperature
    // models (Kimi kimi-for-coding / k3 / kimi-k2.7-code — server enforces
    // temperature=1.0) rejected every attempt, and the last-resort
    // fallback silently DELETED 70% of the context. LLMClient must clamp
    // the temperature it writes into MessageCompressionConfig exactly like
    // the main chat path does.
    CompressionFakeServer server;
    fs::path home = make_temp_home("compress-temp-clamp");

    {
        auto store = hive::SettingsStore::for_home(home);
        hive::ProviderConfig p;
        p.name          = "fake-kimi";
        p.host          = server.base_url();
        p.api_key       = "test-key";
        p.chat_endpoint = "/v1/chat/completions";
        p.plugin        = "moonshot";   // pinned: loopback auto-detects as lmstudio
        store.upsert_provider(p);

        hive::ModelConfig m;
        m.name             = "kimi-fixed-temp";
        m.provider         = "fake-kimi";
        m.model_id         = "kimi-for-coding";   // MoonshotPlugin forces 1.0
        m.max_context_size = 10000;
        m.temperature      = 0.0;   // invalid for this model — clamp must fix it
        store.upsert_model(m);
        store.set_default_model_name("kimi-fixed-temp");
    }

    fs::path project = home / "project";
    fs::create_directories(project);

    hive::AgentConfig cfg;
    cfg.project_root   = project;
    cfg.agent_home  = home;
    cfg.session_id     = "compress-temp-clamp-e2e";
    cfg.stream         = false;
    // memory_enabled stays TRUE — see the on_compaction test above:
    // !memory_enabled wipes messages.json at initialize, deleting the seed.
    cfg.memory_enabled = true;
    cfg.mcp_enabled    = false;
    // cfg.temperature deliberately left at -1.0 → model row's 0.0 applies,
    // which is exactly the value the plugin must clamp to 1.0.

    // §BUG-MEMORY-BLOAT: needs >= 10 non-system messages or the
    // small-history guard skips auto-compress.
    seed_history(project, 12);

    auto sink = std::make_shared<CompactionSink>();
    hive::Agent agent(std::move(cfg), sink);
    agent.initialize();

    agent.submit_user_message("hello");
    agent.run_one_turn();
    agent.shutdown();

    REQUIRE(server.compression_requests() == 1);
    const json body = json::parse(server.last_compression_body());
    // The clamped value must be on the wire — not the store's raw 0.0.
    CHECK(body.value("temperature", -1.0) == 1.0);
    {
        std::lock_guard<std::mutex> lk(sink->mu);
        CHECK(sink->errors.empty());
    }

    fs::remove_all(home);
}


TEST_CASE("Agent::compress_context triggers compression below threshold") {
    // §AutoCompress: host-facing trigger. Usage stays LOW (100/100000), so
    // the 90% auto-trigger can never fire — compression happens only
    // because the host asked for it.
    CompressionFakeServer server(/*first_total=*/100, /*later_total=*/100);
    fs::path home = make_temp_home("host-trigger");

    {
        auto store = hive::SettingsStore::for_home(home);
        hive::ProviderConfig p;
        p.name          = "fake";
        p.host          = server.base_url();
        p.api_key       = "test-key";
        p.chat_endpoint = "/v1/chat/completions";
        p.plugin        = "openai-compatible";
        store.upsert_provider(p);

        hive::ModelConfig m;
        m.name             = "big";
        m.provider         = "fake";
        m.model_id         = "fake-model";
        m.max_context_size = 100000;   // 100 tokens = 0.1% — far below 90%
        m.temperature      = 0.0;
        store.upsert_model(m);
        store.set_default_model_name("big");
    }

    fs::path project = home / "project";
    fs::create_directories(project);

    hive::AgentConfig cfg;
    cfg.project_root   = project;
    cfg.agent_home  = home;
    cfg.session_id     = "host-trigger-e2e";
    cfg.stream         = false;
    cfg.memory_enabled = false;
    cfg.mcp_enabled    = false;

    auto sink = std::make_shared<CompactionSink>();
    hive::Agent agent(std::move(cfg), sink);
    agent.initialize();

    // Set the flag before the turn: it is consumed at the compression
    // checkpoint at the end of the turn's first cycle.
    agent.compress_context();

    agent.submit_user_message("hello");
    agent.run_one_turn();
    agent.shutdown();

    // Compression fired exactly once despite usage far below the 90%
    // threshold; before_tokens reflects the low reported usage.
    {
        std::lock_guard<std::mutex> lk(sink->mu);
        REQUIRE(sink->compactions.size() == 1);
        CHECK(sink->compactions[0].before == 100);
        CHECK(sink->errors.empty());
    }
    CHECK(server.compression_requests() == 1);
    CHECK(server.main_requests() == 2);   // cycle 1 → compress → cycle 2 (DONE)

    fs::remove_all(home);
}


TEST_CASE("Agent::compress_context compresses immediately when idle") {
    // §AutoCompress (immediate-when-idle): with no turn running, the host
    // call must compress synchronously on the caller's thread — no extra
    // turn, no waiting for a future cycle. Usage stays LOW (100/100000)
    // so the 90% auto-trigger can never fire.
    CompressionFakeServer server(/*first_total=*/100, /*later_total=*/100);
    fs::path home = make_temp_home("idle-immediate");

    {
        auto store = hive::SettingsStore::for_home(home);
        hive::ProviderConfig p;
        p.name          = "fake";
        p.host          = server.base_url();
        p.api_key       = "test-key";
        p.chat_endpoint = "/v1/chat/completions";
        p.plugin        = "openai-compatible";
        store.upsert_provider(p);

        hive::ModelConfig m;
        m.name             = "big";
        m.provider         = "fake";
        m.model_id         = "fake-model";
        m.max_context_size = 100000;
        m.temperature      = 0.0;
        store.upsert_model(m);
        store.set_default_model_name("big");
    }

    fs::path project = home / "project";
    fs::create_directories(project);

    hive::AgentConfig cfg;
    cfg.project_root   = project;
    cfg.agent_home  = home;
    cfg.session_id     = "idle-immediate-e2e";
    cfg.stream         = false;
    cfg.memory_enabled = false;
    cfg.mcp_enabled    = false;

    auto sink = std::make_shared<CompactionSink>();
    hive::Agent agent(std::move(cfg), sink);
    agent.initialize();

    // Build a real history: 3 turns → system + 3x(user+assistant) = 7
    // messages, so the 70/30 split has a non-empty middle.
    for (int i = 0; i < 3; ++i) {
        agent.submit_user_message("hello " + std::to_string(i));
        agent.run_one_turn();
    }
    CHECK(server.main_requests() == 3);
    CHECK(server.compression_requests() == 0);

    // Idle: compression must happen NOW, synchronously on this thread.
    agent.compress_context();

    {
        std::lock_guard<std::mutex> lk(sink->mu);
        REQUIRE(sink->compactions.size() == 1);
        // Note: no after<before token assertion — on tiny test histories
        // the rough token estimate can grow (the summary wrapper text
        // outweighs the removed stub messages). Message-count shrink is
        // asserted on the transcript below.
        CHECK(sink->errors.empty());
    }
    CHECK(server.compression_requests() == 1);
    CHECK(server.main_requests() == 3);   // no extra turn was run

    // Transcript rebuilt as system + user(summary) + suffix.
    agent.shutdown();
    {
        const fs::path transcript =
            project / ".hive-agent" / "default" / "messages.json";
        REQUIRE(fs::is_regular_file(transcript));
        std::ifstream f(transcript);
        const json history = json::parse(f);
        REQUIRE(history.size() >= 2);
        CHECK(history.size() < 7);   // first 70% collapsed into the summary
        CHECK(history[0].value("role", "") == "system");
        CHECK(history[1].value("role", "") == "user");
    }

    fs::remove_all(home);
}


TEST_CASE("Agent::compress_context reports fallback delete via on_compaction") {
    // §COMPRESS-REPORT: when ALL summary attempts fail (here: HTTP 500 on
    // every compression request), Messages::compress() falls back to
    // deleting the oldest ~70% WITHOUT a summary. That is a real context
    // reduction and MUST surface as on_compaction — before the fix the
    // fallback returned the same nullopt as a genuine failure, so hosts
    // keying success off on_compaction misreported it ("failed, but the
    // context shrank"). Also asserts the request is NOT carried over to
    // the next turn after a fallback delete.
    CompressionFakeServer server(/*first_total=*/100, /*later_total=*/100,
                                 /*fail_compression=*/true);
    fs::path home = make_temp_home("fallback-report");

    {
        auto store = hive::SettingsStore::for_home(home);
        hive::ProviderConfig p;
        p.name          = "fake";
        p.host          = server.base_url();
        p.api_key       = "test-key";
        p.chat_endpoint = "/v1/chat/completions";
        p.plugin        = "openai-compatible";
        store.upsert_provider(p);

        hive::ModelConfig m;
        m.name             = "big";
        m.provider         = "fake";
        m.model_id         = "fake-model";
        m.max_context_size = 100000;   // usage stays at 0.1% — no auto-trigger
        m.temperature      = 0.0;
        store.upsert_model(m);
        store.set_default_model_name("big");
    }

    fs::path project = home / "project";
    fs::create_directories(project);

    hive::AgentConfig cfg;
    cfg.project_root   = project;
    cfg.agent_home  = home;
    cfg.session_id     = "fallback-report-e2e";
    cfg.stream         = false;
    cfg.memory_enabled = false;
    cfg.mcp_enabled    = false;

    auto sink = std::make_shared<CompactionSink>();
    {
        hive::Agent agent(std::move(cfg), sink);
        agent.initialize();

        // Build a real history: 3 turns → system + 3x(user+assistant) = 7
        // messages, so the fallback delete (needs > 5) can proceed.
        for (int i = 0; i < 3; ++i) {
            agent.submit_user_message("hello " + std::to_string(i));
            agent.run_one_turn();
        }
        CHECK(server.main_requests() == 3);
        CHECK(server.compression_requests() == 0);

        // Idle: compression runs synchronously; all 3 summary attempts get
        // HTTP 500, so compress() takes the fallback-delete path.
        agent.compress_context();

        CHECK(server.compression_requests() == 3);   // 3 attempts, all failed
        CHECK(server.main_requests() == 3);          // no extra turn was run

        // Transcript reduced WITHOUT a summary wrapper: system + last 30%.
        agent.shutdown();
    }

    {
        std::lock_guard<std::mutex> lk(sink->mu);
        // THE FIX: the fallback delete fires on_compaction even though no
        // summary was produced — and after < before because messages were
        // really deleted.
        REQUIRE(sink->compactions.size() == 1);
        CHECK(sink->compactions[0].after < sink->compactions[0].before);
        CHECK(sink->errors.empty());
    }

    {
        const fs::path transcript =
            project / ".hive-agent" / "default" / "messages.json";
        REQUIRE(fs::is_regular_file(transcript));
        std::ifstream f(transcript);
        const json history = json::parse(f);
        CHECK(history.size() < 7);
        CHECK(history[0].value("role", "") == "system");
        // No synthetic "[Context compressed" user message was injected.
        for (const auto& msg : history) {
            CHECK(msg.value("content", "").find("[Context compressed") ==
                  std::string::npos);
        }
    }

    // The fallback delete must NOT re-arm compress_requested_: the next
    // turn runs without compressing again.
    {
        hive::AgentConfig cfg2;
        cfg2.project_root   = project;
        cfg2.agent_home  = home;
        cfg2.session_id     = "fallback-report-e2e";
        cfg2.stream         = false;
        cfg2.memory_enabled = false;
        cfg2.mcp_enabled    = false;

        auto sink2 = std::make_shared<CompactionSink>();
        hive::Agent agent2(std::move(cfg2), sink2);
        agent2.initialize();
        agent2.submit_user_message("are you still there?");
        agent2.run_one_turn();
        agent2.shutdown();

        CHECK(server.main_requests() == 4);        // exactly one new turn
        CHECK(server.compression_requests() == 3); // no second compress
        std::lock_guard<std::mutex> lk(sink2->mu);
        CHECK(sink2->compactions.empty());
    }

    fs::remove_all(home);
}


// ---------------------------------------------------------------------------
// §COMPRESS-TOOL (2026-08-14): model-facing compress_context tool, restored.
// The model emits a compress_context tool call on cycle 1 (usage LOW —
// 100/100000, so the 90% auto-trigger can never fire). The run loop must
// turn the tool result into the same deferred compress request as
// Agent::compress_context(): compression runs at the cycle-boundary
// checkpoint, on_compaction fires once, and the turn completes DONE on
// cycle 2.
// ---------------------------------------------------------------------------
TEST_CASE("model-facing compress_context tool triggers compression below threshold") {
    CompressionFakeServer server(/*first_total=*/100, /*later_total=*/100,
                                 /*fail_compression=*/false,
                                 /*emit_compress_tool_call=*/true);
    fs::path home = make_temp_home("tool-trigger");

    {
        auto store = hive::SettingsStore::for_home(home);
        hive::ProviderConfig p;
        p.name          = "fake";
        p.host          = server.base_url();
        p.api_key       = "test-key";
        p.chat_endpoint = "/v1/chat/completions";
        p.plugin        = "openai-compatible";
        store.upsert_provider(p);

        hive::ModelConfig m;
        m.name             = "big";
        m.provider         = "fake";
        m.model_id         = "fake-model";
        m.max_context_size = 100000;   // 100 tokens = 0.1% — far below 90%
        m.temperature      = 0.0;
        store.upsert_model(m);
        store.set_default_model_name("big");
    }

    fs::path project = home / "project";
    fs::create_directories(project);

    hive::AgentConfig cfg;
    cfg.project_root   = project;
    cfg.agent_home  = home;
    cfg.session_id     = "tool-trigger-e2e";
    cfg.stream         = false;
    cfg.memory_enabled = false;
    cfg.mcp_enabled    = false;

    auto sink = std::make_shared<CompactionSink>();
    hive::Agent agent(std::move(cfg), sink);
    agent.initialize();

    agent.submit_user_message("hello");
    agent.run_one_turn();
    agent.shutdown();

    // The model's own compress_context call scheduled compression at the
    // end of cycle 1 — exactly one compaction, one summary request.
    {
        std::lock_guard<std::mutex> lk(sink->mu);
        REQUIRE(sink->compactions.size() == 1);
        CHECK(sink->compactions[0].before == 100);
        CHECK(sink->errors.empty());
    }
    CHECK(server.compression_requests() == 1);
    CHECK(server.main_requests() == 2);   // cycle 1 (tool call) → compress → cycle 2 (DONE)

    fs::remove_all(home);
}


// ---------------------------------------------------------------------------
// §COMPRESS-TOOL: unit tests for LLMClient::handle_compress_context_result.
// The handler is the seam between the tool result and the deferred
// compress_requested_ flag — verify payload acceptance/rejection.
// ---------------------------------------------------------------------------
namespace {

AppConfig make_compress_handler_config(const fs::path& dir) {
    AppConfig cfg;
    cfg.target_directory = dir;
    cfg.memory_disabled = true;   // hermetic: no SessionMemory on disk
    return cfg;
}

}  // namespace

TEST_SUITE("handle_compress_context_result (§COMPRESS-TOOL)") {

    TEST_CASE("Valid compress_context payload is accepted") {
        LLMClient client(make_compress_handler_config(
            make_temp_home("handler_valid")));
        json result = {{"status", "compress_context"},
                       {"reason", "research complete"}};
        CHECK(client.handle_compress_context_result(result) == true);
    }

    TEST_CASE("Reason is optional for acceptance") {
        LLMClient client(make_compress_handler_config(
            make_temp_home("handler_noreason")));
        CHECK(client.handle_compress_context_result(
                  {{"status", "compress_context"}}) == true);
    }

    TEST_CASE("Rejected payloads return false") {
        LLMClient client(make_compress_handler_config(
            make_temp_home("handler_reject")));
        CHECK(client.handle_compress_context_result(json::object()) == false);
        CHECK(client.handle_compress_context_result(
                  {{"status", "success"}}) == false);
        CHECK(client.handle_compress_context_result(
                  {{"status", "error"}, {"message", "boom"}}) == false);
        CHECK(client.handle_compress_context_result(
                  {{"status", "set_state"}, {"state", "Acting"}}) == false);
        CHECK(client.handle_compress_context_result(json::array()) == false);
    }
}
