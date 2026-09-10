/**
 * @file test_mcp_transport.cpp
 * @brief Unit tests for the MCP transport abstractions
 *
 * Tests:
 * - StdioMcpTransport connects to /bin/cat (echoes stdin to stdout)
 * - HttpMcpTransport requires base_url
 * - TransportError to_string works
 * - Disconnect cleans up
 */

#include <iostream>
#include <chrono>
#include <fstream>
#include <future>
#include <thread>
#include <unistd.h>
#include <sys/stat.h>

#include "mcp/McpTransport.h"
#include "mcp/StdioMcpTransport.h"
#include "mcp/HttpMcpTransport.h"

using namespace mcp;
using namespace std::chrono_literals;

static int failures = 0;
static int test_count = 0;

#define TEST(name) int test_##name()

#define CHECK(cond) do { \
    test_count++; \
    if (!(cond)) { \
        std::cerr << "FAIL: " << #cond << " at " << __FILE__ << ":" << __LINE__ << std::endl; \
        failures++; \
        return 1; \
    } \
} while(0)

#define CHECK_EQ(a, b) do { \
    test_count++; \
    if ((a) != (b)) { \
        std::cerr << "FAIL: " << #a << " == " << #b << " (" << (a) << " vs " << (b) << ") at " << __FILE__ << ":" << __LINE__ << std::endl; \
        failures++; \
        return 1; \
    } \
} while(0)

// ============================================================================
// Test: TransportError to_string
// ============================================================================
TEST(transport_error_to_string) {
    CHECK_EQ(std::string(to_string(TransportError::NotConnected)),      std::string("Not connected"));
    CHECK_EQ(std::string(to_string(TransportError::ConnectionFailed)), std::string("Connection failed"));
    CHECK_EQ(std::string(to_string(TransportError::WriteFailed)),      std::string("Write failed"));
    CHECK_EQ(std::string(to_string(TransportError::ReadFailed)),       std::string("Read failed"));
    CHECK_EQ(std::string(to_string(TransportError::Timeout)),          std::string("Timeout"));
    CHECK_EQ(std::string(to_string(TransportError::InvalidResponse)),  std::string("Invalid response"));
    CHECK_EQ(std::string(to_string(TransportError::Cancelled)),        std::string("Cancelled"));
    CHECK_EQ(std::string(to_string(TransportError::Unsupported)),      std::string("Unsupported operation"));
    return 0;
}

// ============================================================================
// Test: HttpMcpTransport rejects empty base_url
// ============================================================================
TEST(http_transport_requires_base_url) {
    HttpMcpTransport transport("");
    auto result = transport.connect();
    CHECK(!result.has_value());
    if (!result) {
        CHECK(result.error() == TransportError::ConnectionFailed);
    }
    return 0;
}

// ============================================================================
// Test: HttpMcpTransport connect with valid URL
// ============================================================================
TEST(http_transport_connects_with_valid_url) {
    HttpMcpTransport transport("https://mcp.example.com/api");
    auto result = transport.connect();
    CHECK(result.has_value());
    CHECK(transport.is_connected());
    transport.disconnect();
    CHECK(!transport.is_connected());
    return 0;
}

// ============================================================================
// Test: HttpMcpTransport transport_name
// ============================================================================
TEST(http_transport_name) {
    HttpMcpTransport transport("https://example.com");
    CHECK_EQ(transport.transport_name(), std::string("http"));
    return 0;
}

// ============================================================================
// Test: HttpMcpTransport send_request when not connected
// ============================================================================
TEST(http_transport_send_request_not_connected) {
    HttpMcpTransport transport("https://example.com");
    // Don't connect
    auto fut = transport.send_request("test", nlohmann::json::object());
    auto result = fut.get();
    CHECK(!result.has_value());
    if (!result) {
        CHECK(result.error() == TransportError::NotConnected);
    }
    return 0;
}

// ============================================================================
// Test: StdioMcpTransport to /bin/cat (echo test)
// ============================================================================
TEST(stdio_transport_echo_with_cat) {
    StdioMcpTransport transport("/bin/cat");
    auto result = transport.connect();
    if (!result) {
        std::cerr << "SKIP: /bin/cat not available or fork failed\n";
        return 0;
    }

    // /bin/cat echoes stdin to stdout. Send a JSON-RPC-like line and verify
    // it gets echoed back. NOTE: /bin/cat doesn't speak JSON-RPC, so the
    // response will be the raw text, but it should be received.
    auto fut = transport.send_request("ping", {{"id", 42}});
    auto status = fut.wait_for(2s);
    if (status == std::future_status::timeout) {
        std::cerr << "SKIP: cat didn't respond\n";
        transport.disconnect();
        return 0;
    }

    auto response = fut.get();
    // /bin/cat will echo back what we send, but our pending_ lookup will
    // fail because the response format isn't JSON-RPC with a matching id.
    // So we expect either:
    // 1. InvalidResponse error (if our parser tried to parse it)
    // 2. Timeout (if we wait forever)
    // For this test, we just verify we got SOME response within timeout.
    (void)response;
    transport.disconnect();
    return 0;
}

// ============================================================================
// Regression test: JSON-RPC round trip over the stdio pipe
// ============================================================================
// §fix (2026-07-26): write_all() used send(fd, ..., MSG_NOSIGNAL), which
// fails with ENOTSOCK on a pipe fd — so no stdio MCP server ever received
// a request. The echo test above didn't catch it because it accepted ANY
// outcome (including WriteFailed) as a pass. This test requires a real
// response to come back through the pipe.
//
// The fake server is a script (not an inline one-liner) because the
// transport re-quotes the command for `sh -c`; GNU sed -u gives us
// line-buffered, id-echoing JSON-RPC responses with no quoting pitfalls.
static std::string write_fake_mcp_server_script() {
    std::string path = "/tmp/hive_fake_mcp_server_" +
                       std::to_string(getpid()) + ".sh";
    {
        std::ofstream f(path);
        f << "#!/bin/sh\n"
             "exec sed -u -n 's/.*\"id\":\\([0-9][0-9]*\\).*/"
             "{\"jsonrpc\":\"2.0\",\"id\":\\1,"
             "\"result\":{\"ok\":true}}/p'\n";
    }
    chmod(path.c_str(), 0755);
    return path;
}

TEST(stdio_transport_jsonrpc_round_trip_over_pipe) {
    std::string script = write_fake_mcp_server_script();
    StdioMcpTransport transport(script);
    auto result = transport.connect();
    CHECK(result.has_value());

    auto fut = transport.send_request("ping", nlohmann::json::object(),
                                      /*timeout_ms=*/5000);
    auto status = fut.wait_for(6s);
    CHECK(status == std::future_status::ready);
    if (status == std::future_status::ready) {
        auto response = fut.get();
        // WriteFailed here is the ENOTSOCK regression. The future
        // resolves with the unwrapped "result" payload (reader_loop).
        CHECK(response.has_value());
        if (response) {
            CHECK(response->value("ok", false) == true);
        }
    }
    transport.disconnect();
    unlink(script.c_str());
    return 0;
}

// ============================================================================
// Test: StdioMcpTransport transport_name
// ============================================================================
TEST(stdio_transport_name) {
    StdioMcpTransport transport("/bin/cat");
    CHECK_EQ(transport.transport_name(), std::string("stdio"));
    return 0;
}

// ============================================================================
// Test: StdioMcpTransport disconnect cleans up
// ============================================================================
TEST(stdio_transport_disconnect) {
    StdioMcpTransport transport("/bin/cat");
    auto result = transport.connect();
    if (!result) {
        std::cerr << "SKIP: /bin/cat not available\n";
        return 0;
    }
    CHECK(transport.is_connected());
    transport.disconnect();
    CHECK(!transport.is_connected());
    return 0;
}

// ============================================================================
// Test: StdioMcpTransport fails pending requests when the child exits
// ============================================================================
// §fix (2026-07-18): reader_loop used to check only POLLIN, so a dead
// child (POLLHUP) made it spin at 100% CPU and pending requests hung
// until their deadline. Now the pending future resolves promptly with
// an error.
TEST(stdio_transport_child_exit_fails_pending) {
    // "true" exits immediately without writing anything.
    StdioMcpTransport transport("true");
    auto result = transport.connect();
    if (!result) {
        std::cerr << "SKIP: fork failed\n";
        return 0;
    }

    // Long timeout — the point is the future resolves LONG before it.
    auto fut = transport.send_request("ping", nlohmann::json::object(),
                                      /*timeout_ms=*/30000);
    auto status = fut.wait_for(5s);
    CHECK(status == std::future_status::ready);
    if (status == std::future_status::ready) {
        auto response = fut.get();
        CHECK(!response.has_value());
        if (!response) {
            // ReadFailed when the reader thread saw POLLHUP; WriteFailed
            // is also acceptable (child died before our write → EPIPE).
            CHECK(response.error() == TransportError::ReadFailed ||
                  response.error() == TransportError::WriteFailed);
        }
    }
    transport.disconnect();
    return 0;
}

// ============================================================================
// Test: Virtual dispatch through base pointer
// ============================================================================
TEST(virtual_dispatch) {
    std::unique_ptr<McpTransport> t = std::make_unique<HttpMcpTransport>("https://example.com");
    auto result = t->connect();
    CHECK(result.has_value());
    CHECK_EQ(t->transport_name(), std::string("http"));
    t->disconnect();
    return 0;
}

// ============================================================================
// MAIN
// ============================================================================

int main() {
    std::cout << "Running McpTransport tests..." << std::endl;

    int local_failures = 0;
    local_failures += test_transport_error_to_string();
    local_failures += test_http_transport_requires_base_url();
    local_failures += test_http_transport_connects_with_valid_url();
    local_failures += test_http_transport_name();
    local_failures += test_http_transport_send_request_not_connected();
    local_failures += test_stdio_transport_echo_with_cat();
    local_failures += test_stdio_transport_jsonrpc_round_trip_over_pipe();
    local_failures += test_stdio_transport_name();
    local_failures += test_stdio_transport_disconnect();
    local_failures += test_stdio_transport_child_exit_fails_pending();
    local_failures += test_virtual_dispatch();

    std::cout << "\nResults: " << (local_failures == 0 ? "PASS" : "FAIL") << std::endl;
    std::cout << "Total checks: " << test_count << std::endl;
    std::cout << "Failed checks: " << local_failures << std::endl;

    return local_failures > 0 ? 1 : 0;
}
