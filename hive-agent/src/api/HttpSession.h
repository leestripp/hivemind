/**
 * @file HttpSession.h
 * @brief Async HTTP client — OmniNet-backed adapter
 *
 * Implementation in HttpSession.cpp uses OmniNet::HTTP::Client directly.
 * Public API: HttpRequest / HttpResponse / HttpError / submit /
 * submit_cancellable / submit_batch / get_stats / shutdown.
 *
 * The "Http" prefix replaces the legacy "Curl" name; this is now an
 * OmniNet wrapper, not a libcurl one.
 *
 * Architecture:
 *   - submit() returns a std::future backed by a per-request jthread.
 *     Each request uses OmniNet::HTTP::Client directly — no shared
 *     event-loop thread. Connection pooling comes from OmniNet's
 *     singleton Client (it reuses TLS sockets).
 *   - submit_cancellable() honours the std::stop_token by passing it
 *     through to OmniNet::send(req, stop_token) and polling it on the
 *     worker thread so cancellation latency is bounded.
 *   - SSE streaming: when HttpRequest::stream_callback is set, the
 *     worker calls OmniNet::HTTP::Client::send_streaming and pulls
 *     chunks, invoking the callback per chunk.
 *
 * Thread-safety:
 *   - submit / submit_cancellable / submit_batch are safe from any thread.
 *   - get_stats is lock-free (atomic counters).
 *   - shutdown() is idempotent.
 *
 * Usage:
 * @code
 *   auto& session = HttpSession::get_instance();
 *   HttpRequest req;
 *   req.url = "https://api.example.com/data";
 *   req.method = "GET";
 *   req.bearer_token = "sk-...";
 *   req.timeout_seconds = 30;
 *
 *   auto fut = session.submit_cancellable(req, stop_token);
 *   auto result = fut.get();
 *   if (result) {
 *       std::cout << "Status: " << result->status_code << "\n";
 *       std::cout << "Body: " << result->body << "\n";
 *   } else {
 *       std::cerr << "Error: " << to_string(result.error()) << "\n";
 *   }
 * @endcode
 */

#pragma once

#include <atomic>
#include <cstdint>
#include <chrono>
#include <expected>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <queue>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace hive::net {

// ============================================================================
// Request / Response types
// ============================================================================

/**
 * @brief HTTP request configuration
 */
struct HttpRequest {
    std::string url;                 ///< Target URL (required)
    std::string method = "GET";      ///< HTTP method (GET, POST, PUT, DELETE, etc.)
    std::string body;                ///< Request body (for POST/PUT)
    std::vector<std::string> headers; ///< Extra headers (e.g., "Content-Type: application/json")
    std::string bearer_token;        ///< Optional bearer token (added as Authorization: Bearer ...)
    long timeout_seconds = 300;      ///< Total request timeout (default 5 min)
    bool follow_redirects = true;    ///< Follow 3xx redirects
    std::string user_agent = "hive-brain/1.0";  ///< User-Agent header

    /// Optional streaming callback. When set, the write callback invokes it
    /// for each chunk of data received (SSE streaming). The response.body
    /// is NOT accumulated in this mode. The caller is responsible for
    /// parsing SSE lines and extracting text deltas.
    std::function<void(std::string_view)> stream_callback;
};

/**
 * @brief HTTP response
 */
struct HttpResponse {
    long status_code = 0;            ///< HTTP status (200, 404, etc.)
    std::string body;                ///< Response body
    std::string content_type;        ///< Content-Type header (if any)
    /// MCP streamable-HTTP session ID (read from the Mcp-Session-Id
    /// response header by the curl header callback). Empty if the server
    /// didn't send one. Used by HttpMcpTransport to replay the header on
    /// every subsequent request per the MCP 2025-06-18 streamable-HTTP
    /// spec §Session Management.
    std::string mcp_session_id;
    bool timed_out = false;          ///< True if request timed out
    bool cancelled = false;          ///< True if cancelled via stop_token
    /// §B14 fix: Retry-After header value (seconds, lowercased header name).
    /// Only populated for 429 / 503 responses per RFC 7231 §7.1.3.
    /// Empty if not present.
    int retry_after_seconds = 0;

    [[nodiscard]] bool ok() const noexcept { return status_code >= 200 && status_code < 300; }
};

/**
 * @brief Error categories for HTTP operations
 */
enum class HttpError : std::uint8_t {
    InitFailed,         ///< OmniNet HTTP client init failed
    InvalidUrl,         ///< URL is empty or malformed
    Network,            ///< DNS, connection refused, etc.
    Timeout,            ///< Operation exceeded timeout_seconds
    Cancelled,          ///< Cancelled via stop_token
    HttpStatus,         ///< Non-2xx response (use response.status_code for details)
    WriteError,         ///< Write callback failed
    Unknown,            ///< Other HTTP transport error
};

/**
 * @brief Convert HttpError to human-readable string
 */
inline const char* to_string(HttpError e) {
    switch (e) {
        case HttpError::InitFailed:   return "Failed to initialize HTTP client";
        case HttpError::InvalidUrl:   return "Invalid or empty URL";
        case HttpError::Network:      return "Network error";
        case HttpError::Timeout:      return "Request timed out";
        case HttpError::Cancelled:    return "Request cancelled";
        case HttpError::HttpStatus:   return "HTTP error status";
        case HttpError::WriteError:   return "Write callback error";
        case HttpError::Unknown:      return "Unknown error";
    }
    return "Unknown error";
}

// ============================================================================
// HttpSession
// ============================================================================

/**
 * @brief Async HTTP session backed by OmniNet
 *
 * Singleton per process. Use get_instance() to access.
 */
class HttpSession {
public:
    static HttpSession& get_instance();

    // Non-copyable
    HttpSession(const HttpSession&) = delete;
    HttpSession& operator=(const HttpSession&) = delete;

    /**
     * @brief Submit a request and get a future to the result
     *
     * The request is queued and processed by the event-loop thread. Use
     * the returned future to await the result.
     *
     * @param request Request configuration
     * @return std::future<std::expected<HttpResponse, HttpError>>
     */
    std::future<std::expected<HttpResponse, HttpError>> submit(const HttpRequest& request);

    /**
     * @brief Submit a request that can be cancelled via stop_token
     *
     * When the stop_token is requested, the worker thread cancels
     * the OmniNet request and fulfills the future with HttpError::Cancelled.
     */
    std::future<std::expected<HttpResponse, HttpError>> submit_cancellable(
        const HttpRequest& request,
        std::stop_token stop_token
    );

    /**
     * @brief Submit multiple requests concurrently (batch)
     *
     * All requests are in-flight at once. Use the returned futures to
     * await each result. Useful for parallel tool calls.
     */
    std::vector<std::future<std::expected<HttpResponse, HttpError>>> submit_batch(
        const std::vector<HttpRequest>& requests
    );

    /**
     * @brief Get session statistics
     */
    struct Stats {
        size_t active;       ///< Requests currently in-flight
        size_t completed;    ///< Total requests completed (cumulative)
        size_t total;        ///< Total requests ever submitted
    };
    Stats get_stats() const;

    /**
     * @brief Graceful shutdown
     *
     * Stops accepting new requests, cancels in-flight ones, joins the
     * event loop thread. Safe to call multiple times.
     */
    void shutdown();

    ~HttpSession();

private:
    HttpSession();
    // §NC-6: legacy event-loop helpers are no-ops now. The implementation
    // in HttpSession.cpp uses per-request jthreads and OmniNet directly.
    void event_loop(std::stop_token st);
    struct RequestState;     // forward-declared for ABI; no longer used
    void process_completed();
    void add_to_multi(RequestState* state);
    void remove_from_multi(RequestState* state, HttpError error);
    static size_t write_cb(void* contents, size_t size, size_t nmemb, void* userp);
    static size_t header_cb(void* contents, size_t size, size_t nmemb, void* userp);

    // Worker thread + stop source for the legacy event loop. With the
    // OmniNet rewrite these are unused but kept for ABI.
    std::jthread worker_;
    std::stop_source stop_source_;

    // Legacy queue + in-flight tracking. Kept as members but never
    // populated; the dtor's member-cleanup needs them to compile.
    mutable std::mutex queue_mutex_;
    std::condition_variable queue_cv_;
    std::queue<std::unique_ptr<RequestState>> pending_;
    std::vector<RequestState*> in_flight_;
    std::mutex in_flight_mutex_;

    std::atomic<bool> stop_{false};
    std::atomic<size_t> active_count_{0};
    std::atomic<size_t> completed_count_{0};
    std::atomic<size_t> total_count_{0};
};

} // namespace hive::net
