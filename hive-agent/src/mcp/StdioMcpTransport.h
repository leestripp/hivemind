/**
 * @file StdioMcpTransport.h
 * @brief stdio-based MCP transport (current implementation)
 */

#pragma once

#include "McpTransport.h"

#include <atomic>
#include <condition_variable>
#include <future>
#include <map>
#include <mutex>
#include <queue>

namespace mcp {

/**
 * @brief stdio transport (subprocess with stdin/stdout pipes)
 *
 * - Spawns a subprocess and communicates via stdin/stdout
 * - Uses a background reader thread to match responses to requests
 * - Supports concurrent requests via promise/future matching
 */
class StdioMcpTransport : public McpTransport {
public:
    StdioMcpTransport(std::string command,
                      std::map<std::string, std::string> env = {});
    ~StdioMcpTransport() override;

    // Non-copyable
    StdioMcpTransport(const StdioMcpTransport&) = delete;
    StdioMcpTransport& operator=(const StdioMcpTransport&) = delete;

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

    std::string transport_name() const override { return "stdio"; }
    bool health_check() override;

private:
    struct PendingRequest {
        std::promise<std::expected<nlohmann::json, TransportError>> promise;
        std::chrono::steady_clock::time_point deadline;
    };

    // Reader thread: reads stdout, matches responses to pending requests
    void reader_loop();

    // Internal state
    std::string command_;
    std::map<std::string, std::string> env_;
    pid_t child_pid_ = -1;
    int stdin_write_ = -1;   // We write to child's stdin
    int stdout_read_ = -1;   // We read from child's stdout

    std::atomic<bool> connected_{false};
    std::atomic<bool> stop_{false};
    std::thread reader_thread_;

    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::map<int, std::shared_ptr<PendingRequest>> pending_;  // by request ID
    int next_request_id_ = 1;
};

} // namespace mcp
