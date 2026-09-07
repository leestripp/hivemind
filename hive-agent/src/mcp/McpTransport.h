/**
 * @file McpTransport.h
 * @brief Abstract transport interface for MCP (Model Context Protocol)
 *
 * Why this exists:
 * - MCP supports multiple transports: stdio (current), HTTP+SSE, streamable HTTP
 * - Adding new transports should not require changes to McpClient core logic
 * - Transport implementations are interchangeable behind a common interface
 *
 * Usage:
 * @code
 *   auto transport = std::make_unique<mcp::StdioMcpTransport>();
 *   transport->connect(command, env);
 *   auto response = transport->send_request("tools/list", {}).get();
 * @endcode
 */

#pragma once

#include <cstdint>
#include <expected>
#include <future>
#include <map>
#include <nlohmann/json.hpp>
#include <string>

namespace mcp {

/**
 * @brief Transport-level errors
 */
enum class TransportError : std::uint8_t {
    NotConnected,
    ConnectionFailed,
    WriteFailed,
    ReadFailed,
    Timeout,
    InvalidResponse,
    Cancelled,
    Unsupported,
};

/**
 * @brief Convert TransportError to string
 */
inline const char* to_string(TransportError e) {
    switch (e) {
        case TransportError::NotConnected:      return "Not connected";
        case TransportError::ConnectionFailed: return "Connection failed";
        case TransportError::WriteFailed:      return "Write failed";
        case TransportError::ReadFailed:       return "Read failed";
        case TransportError::Timeout:          return "Timeout";
        case TransportError::InvalidResponse:  return "Invalid response";
        case TransportError::Cancelled:        return "Cancelled";
        case TransportError::Unsupported:      return "Unsupported operation";
    }
    return "Unknown error";
}

/**
 * @brief Abstract MCP transport interface
 *
 * A transport is responsible for:
 * 1. Establishing a connection to the MCP server
 * 2. Sending JSON-RPC requests
 * 3. Receiving JSON-RPC responses (matching by ID)
 * 4. Cleaning up the connection
 */
class McpTransport {
public:
    virtual ~McpTransport() = default;

    /**
     * @brief Connect to the MCP server
     * @return std::expected<void, TransportError>
     */
    virtual std::expected<void, TransportError> connect() = 0;

    /**
     * @brief Disconnect and clean up resources
     */
    virtual void disconnect() = 0;

    /**
     * @brief Check if currently connected
     */
    [[nodiscard]] virtual bool is_connected() const = 0;

    /**
     * @brief Send a JSON-RPC request and get the response (async)
     *
     * @param method JSON-RPC method name (e.g., "tools/call")
     * @param params Method parameters
     * @param timeout_ms Timeout in milliseconds (0 = no timeout)
     * @return std::future<expected<json, TransportError>>
     */
    virtual std::future<std::expected<nlohmann::json, TransportError>> send_request(
        const std::string& method,
        const nlohmann::json& params,
        int timeout_ms = 30000
    ) = 0;

    /**
     * @brief Send a JSON-RPC notification (no response expected)
     */
    virtual std::expected<void, TransportError> send_notification(
        const std::string& method,
        const nlohmann::json& params
    ) = 0;

    /**
     * @brief Get the transport name (for logging)
     */
    [[nodiscard]] virtual std::string transport_name() const = 0;

    /**
     * @brief Health check - verify the transport is still functional
     * @return true if healthy, false if connection is broken
     *
     * Implementations should:
     * - Check internal connection state
     * - Optionally send a lightweight ping/initialize
     * - Return false quickly if the connection is dead
     *
     * Used by the manager to decide when to reconnect.
     */
    virtual bool health_check() = 0;

    /**
     * @brief Reconnect after a connection failure
     * @return true if reconnected successfully
     *
     * Default implementation calls disconnect() then connect().
     * Transports can override for smarter reconnection logic.
     */
    virtual bool reconnect() {
        disconnect();
        return connect().has_value();
    }
};

} // namespace mcp
