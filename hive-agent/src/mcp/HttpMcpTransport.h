/**
 * @file HttpMcpTransport.h
 * @brief HTTP-based MCP transport (streamable HTTP / SSE)
 *
 * Uses the HttpSession for async HTTP I/O.
 * Supports streamable HTTP and SSE (Server-Sent Events) transports.
 */

#pragma once

#include "McpTransport.h"

#include <nlohmann/json.hpp>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <atomic>
#include <map>
#include <future>

namespace mcp {

/**
 * @brief HTTP transport for MCP (Streamable HTTP)
 *
 * Sends JSON-RPC requests as HTTP POST and reads responses.
 * Uses HttpSession for async I/O.
 */
class HttpMcpTransport : public McpTransport {
public:
    /**
     * @param base_url Base URL of the MCP server (e.g., "https://mcp.example.com")
     * @param bearer_token Optional bearer token for auth
     */
    HttpMcpTransport(std::string base_url, std::string bearer_token = "");

    ~HttpMcpTransport() override;

    // Non-copyable
    HttpMcpTransport(const HttpMcpTransport&) = delete;
    HttpMcpTransport& operator=(const HttpMcpTransport&) = delete;

    std::expected<void, TransportError> connect() override;
    void disconnect() override;
    bool is_connected() const override { return connected_.load(); }

    std::future<std::expected<nlohmann::json, TransportError>> send_request(
        const std::string& method,
        const nlohmann::json& params,
        int timeout_ms = 30000
    ) override;

    std::expected<void, TransportError> send_notification(
        const std::string& method,
        const nlohmann::json& params
    ) override;

    std::string transport_name() const override { return "http"; }
    bool health_check() override;

private:
    std::string base_url_;
    std::string bearer_token_;

    std::atomic<bool> connected_{false};

    // §18.9 — MCP streamable-HTTP session continuity. apollocore (and
    // other spec-compliant servers) assign a session id at `initialize`
    // time (returned in the Mcp-Session-Id response header) and reject
    // every subsequent POST that omits it with HTTP 400. We cache it
    // here and replay it on every request/notification below.
    std::mutex session_id_mu_;
    std::string session_id_;
};

} // namespace mcp
