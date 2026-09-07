#pragma once

#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <vector>
#include <map>
#include <optional>
#include <nlohmann/json.hpp>
#include <future>
#include <stop_token>

using json = nlohmann::json;

// Forward declaration
class Subprocess;

// Internal structure to hold pipe handles
struct McpPipes {
    int stdin_write;
    int stdout_read;
    pid_t child_pid;
    
    McpPipes() : stdin_write(-1), stdout_read(-1), child_pid(-1) {}
};

namespace mcp {

// Tool definition from MCP server
struct ToolDefinition {
    std::string name;
    std::string description;
    json input_schema;
};

// Tool call result
struct ToolResult {
    bool success;
    std::string content;
    std::optional<std::string> error;
};

// MCP client that communicates with MCP server via stdio transport
class McpClient {
public:
    McpClient();
    ~McpClient();
    
    // Disable copying
    McpClient(const McpClient&) = delete;
    McpClient& operator=(const McpClient&) = delete;
    
    // Connect to MCP server (stdio transport)
    // Returns true on success
    bool connect(const std::string& command,
                 const std::map<std::string, std::string>& env = {},
                 int timeout_ms = 10000);
    
    // Disconnect from MCP server
    void disconnect();
    
    // Check if connected
    bool is_connected() const;
    
    // Initialize MCP session (send capabilities, receive server info)
    bool initialize();
    
    // List available tools from the server
    std::vector<ToolDefinition> list_tools();
    
    // Call a tool with arguments
    ToolResult call_tool(const std::string& tool_name, const json& arguments);

    // Async version of call_tool - returns a future. Useful for parallel tool
    // execution and cancellation via stop_token.
    std::future<ToolResult> call_tool_async(
        const std::string& tool_name,
        const json& arguments,
        std::stop_token stop_token = {}
    );

    // Batch call - execute multiple tool calls in parallel via ThreadPool.
    // Returns futures in the same order as the input. Independent calls run
    // concurrently (saves time when one call would block waiting for another).
    struct ToolCallSpec {
        std::string tool_name;
        json arguments;
    };
    std::vector<std::future<ToolResult>> call_tools_batch(
        const std::vector<ToolCallSpec>& calls
    );
    
    // Specifically for MiniMax's understand_image tool
    std::string understand_image(const std::string& image_url,
                                  const std::string& prompt = "Describe this image in detail.");
    
    // Web search via MiniMax's web_search tool
    json web_search(const std::string& query, int limit = 10);
    
    // Get last error message
    std::string get_last_error() const;

    // §9.7.C — Register a callback for `notifications/tools/list_changed`.
    // The callback is fired from the SAME thread that calls read_response()
    // (i.e., the caller's thread) when such a notification is observed in
    // the byte stream. Returns the previous callback (if any) so callers
    // can chain.
    //
    // Note: this replaces the old separate-listener-thread design, which
    // raced with the main reader on the shared stdout pipe and could
    // silently consume bytes that belonged to a real JSON-RPC response
    // (causing the caller to hang until timeout).
    using ListChangedCallback = std::function<void()>;
    ListChangedCallback set_list_changed_callback(ListChangedCallback cb) {
        std::scoped_lock lk(cb_mu_);
        auto prev = std::move(list_changed_cb_);
        list_changed_cb_ = std::move(cb);
        return prev;
    }
    void clear_list_changed_callback() {
        std::scoped_lock lk(cb_mu_);
        list_changed_cb_ = nullptr;
    }

private:
    // Internal helper to send JSON-RPC request and get response.
    // §MCP-EOF: returns null json on ANY failure (write error, EOF,
    // timeout, ...); callers distinguish failure via is_null().
    json send_request(const std::string& method, const json& params, int timeout_ms = 30000);
    
    // Internal helper to send notification (no response expected)
    void send_notification(const std::string& method, const json& params);
    
    // Read response from server with timeout. When expected_id >= 0,
    // only the JSON-RPC frame whose "id" matches is returned;
    // notifications and stale/mismatched frames are discarded while
    // reading continues (until the overall timeout).
    json read_response(int timeout_ms = 30000, int expected_id = -1);
    
    // Parse SSE stream data
    std::string parse_sse_data(const std::string& line);
    
    // Read all available output (for SSE responses)
    std::string read_all_output(int timeout_ms = 30000);
    
    // The pipe handles
    McpPipes* pipes_;
    
    // Connection state
    bool connected_;
    bool initialized_;
    
    // Server info
    std::string server_version_;
    
    // Last error
    std::string last_error_;

    // §9.7.C — list_changed callback. Fired inline from read_response()
    // when a notifications/tools/list_changed notification is observed.
    // Mutex-guarded because set_list_changed_callback() may be called
    // from a different thread than read_response() runs on.
    mutable std::mutex cb_mu_;
    ListChangedCallback list_changed_cb_;

    // Request ID counter. Guarded by request_mu_ (see below).
    int next_request_id_;

    // §fix (2026-07-18): serializes send_request() — id allocation, the
    // pipe write, and the matching read_response() — so concurrent
    // call_tool_async()/call_tools_batch() on one client can't interleave
    // frames on the shared stdio pipe.
    std::mutex request_mu_;

    // Available tools cache
    std::vector<ToolDefinition> tools_;
};

} // namespace mcp