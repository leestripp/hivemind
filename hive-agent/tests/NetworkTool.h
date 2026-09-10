#pragma once

#include "tools/BaseTool.h"
#include <memory>
#include <string>
#include <map>
#include <mutex>
#include <thread>
#include <condition_variable>
#include <unordered_map>
#include <optional>
#include <vector>
#include <chrono>
#include <atomic>

// Forward declarations to keep the header light — full OmniNet WebSocket
// types are pulled in only by NetworkTool.cpp.
namespace OmniNet::WebSocket {
    class Connection;
}

/**
 * HttpRequestTool - HTTP/HTTPS client tool backed by OmniNet.
 *
 * Replaces Bash `curl ...` and `python3 -c "import urllib..."` for one-shot
 * REST requests. Supports GET/POST/PUT/DELETE/PATCH/PATCH/HEAD/OPTIONS.
 * HTTPS uses OmniNet's TLSSocket with SNI + ALPN (h2, http/1.1) and the
 * system CA bundle for verification. Use `insecure: true` to disable
 * verification (prints a [WARN] to stderr).
 *
 * Body cap is 5 MB by default; larger responses are truncated and the
 * truncated flag is set. Use `max_response_bytes` to override.
 *
 * Per-tool timeout is enforced via setRecvTimeout/setSendTimeout on the
 * underlying socket (default 30s, max 300s).
 *
 * Notes:
 *   - No redirects are followed automatically. Status 3xx is returned as-is.
 *   - HTTP/1.1 only. OmniNet doesn't ship an HTTP/2 client yet.
 *   - Uses OmniNet for connection + TLS, but parses responses with its own
 *     minimal parser since OmniNet's Request parser is hardcoded for
 *     request lines (METHOD path VERSION).
 */

/**
 * DnsLookupTool - Resolve a hostname to IP addresses.
 *
 * Uses POSIX getaddrinfo() under the hood (which respects /etc/hosts,
 * /etc/resolv.conf, nsswitch.conf, etc.). Returns A (IPv4) and AAAA
 * (IPv6) records. No network connection is made beyond the resolver.
 */

/**
 * TcpCheckTool - Test TCP reachability of host:port.
 *
 * Tries to open a socket with the given timeout. If connect() succeeds,
 * closes the socket immediately and reports success. Reports failure
 * with the underlying error otherwise.
 *
 * Useful for: checking if a service is up before calling http_request
 * against it, debugging network issues, validating firewall rules.
 */

/**
 * TlsInspectTool - Inspect a TLS server's certificate chain and connection.
 *
 * Connects to host:port over TLS, then reports:
 *   - negotiated TLS version and cipher
 *   - peer certificate subject, issuer, validity dates
 *   - signature algorithm, public key algorithm and size
 *   - subjectAltName entries (DNS names + IPs)
 *   - SHA-256 fingerprint
 *   - days until expiry
 *   - whether the chain is trusted by the system CA store
 *
 * Does not send any HTTP request — the TLS handshake is enough to get all
 * this info. Closes the connection immediately after.
 */
class HttpRequestTool : public BaseTool {
public:
    std::string get_name() const override { return "http_request"; }
    std::string get_description() const override;
    json get_definition() const override;
    json execute(const json& args, FileIO& file_io) override;
    // §Phase C2: 3-arg overload for streaming tool-call progress. The
    // 2-arg version above delegates here with `progress=nullptr`.
    // "connecting" fires before the network round-trip; "receiving"
    // fires once the body is read (the long part for big downloads).
    json execute(const json& args, FileIO& file_io, ToolProgressFn progress) override;

private:
    struct ParsedUrl {
        std::string scheme;  // "http" or "https" (lowercase)
        std::string host;
        uint16_t port = 0;
        std::string path;    // includes query string; defaults to "/" if empty
        bool is_tls = false;
    };

    // Parse http(s)://host[:port][/path][?query]
    // Returns nullopt on malformed URL or non-http(s) scheme.
    static std::optional<ParsedUrl> parse_url(const std::string& url);

    // Send a single HTTP request. Returns JSON:
    //   {status, status_text, headers, body, body_truncated,
    //    final_url, elapsed_ms, body_bytes}
    // On connection / TLS / parse error returns:
    //   {error, final_url, elapsed_ms}
    // `progress` (optional) is invoked with byte-level progress during
    // the body read. Emits at fixed milestones (25/50/75/100%) for
    // Content-Length responses, and just start/end for chunked.
    static json do_request(const ParsedUrl& url,
                           const std::string& method,
                           const std::map<std::string,std::string>& headers,
                           const std::string& body,
                           int timeout_seconds,
                           size_t max_response_bytes,
                           bool insecure,
                           ToolProgressFn progress = nullptr);
};

class DnsLookupTool : public BaseTool {
public:
    std::string get_name() const override { return "dns_lookup"; }
    std::string get_description() const override;
    json get_definition() const override;
    json execute(const json& args, FileIO& file_io) override;
};

class TcpCheckTool : public BaseTool {
public:
    std::string get_name() const override { return "tcp_check"; }
    std::string get_description() const override;
    json get_definition() const override;
    json execute(const json& args, FileIO& file_io) override;
};

class TlsInspectTool : public BaseTool {
public:
    std::string get_name() const override { return "tls_inspect"; }
    std::string get_description() const override;
    json get_definition() const override;
    json execute(const json& args, FileIO& file_io) override;
};

// ============================================================================
// WebSocket tools (P4)
//
// All four share a singleton ConnectionRegistry that holds the live
// OmniNet::WebSocket::Connection objects. Each connection has a string ID
// like "ws_0x7f3a2b" that the LLM passes between tool calls. Idle
// connections (no send/recv for >5 min) are evicted by the registry.
// ============================================================================
class WebSocketConnectTool : public BaseTool {
public:
    std::string get_name() const override { return "websocket_connect"; }
    std::string get_description() const override;
    json get_definition() const override;
    json execute(const json& args, FileIO& file_io) override;
};

class WebSocketSendTool : public BaseTool {
public:
    std::string get_name() const override { return "websocket_send"; }
    std::string get_description() const override;
    json get_definition() const override;
    json execute(const json& args, FileIO& file_io) override;
};

class WebSocketRecvTool : public BaseTool {
public:
    std::string get_name() const override { return "websocket_recv"; }
    std::string get_description() const override;
    json get_definition() const override;
    json execute(const json& args, FileIO& file_io) override;
};

class WebSocketCloseTool : public BaseTool {
public:
    std::string get_name() const override { return "websocket_close"; }
    std::string get_description() const override;
    json get_definition() const override;
    json execute(const json& args, FileIO& file_io) override;
};

// ============================================================================
// WebSocket connection registry (singleton, lives in NetworkTool.cpp)
//
// Holds live OmniNet::WebSocket::Connection objects keyed by string id.
// One instance for the entire process; the four WebSocket tools call into
// it. Idle connections are evicted by a background reaper thread (5 min).
// ============================================================================
class WsRegistry {
public:
    static WsRegistry& instance();

    // Open a new WebSocket connection. Returns the assigned id.
    // On failure, returns nullopt + error string.
    std::optional<std::string> connect(const std::string& url,
                                       std::string* err);

    // Look up an existing connection. Returns nullptr if not found.
    // Returns std::shared_ptr so the caller holds a reference even if
    // the reaper thread erases the entry from the map while the caller
    // is using it. This prevents a use-after-free.
    // Marks the connection as recently used.
    struct Entry {
        std::unique_ptr<OmniNet::WebSocket::Connection> ws;
        std::string url;
        std::chrono::steady_clock::time_point last_used;
        std::atomic<bool> open{true};
    };
    std::shared_ptr<Entry> get(const std::string& id);

    // List all connection ids.
    std::vector<std::string> list_ids() const;

    // Close + remove a connection. Returns true if it existed.
    bool close(const std::string& id, int code, const std::string& reason);

    // For tests: get the count without taking the lock for long.
    size_t size_unsafe() const;

private:
    WsRegistry();
    ~WsRegistry();
    WsRegistry(const WsRegistry&) = delete;
    WsRegistry& operator=(const WsRegistry&) = delete;

    void start_reaper();
    void stop_reaper();

    mutable std::mutex mu_;
    std::unordered_map<std::string, std::shared_ptr<Entry>> conns_;
    std::atomic<uint64_t> next_id_{1};

    std::thread reaper_;
    std::atomic<bool> reaper_running_{false};
    std::condition_variable reaper_cv_;
};