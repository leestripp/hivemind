#include "NetworkTool.h"
#include "core/Logger.h"

#include "core/FileIO.h"

#include <OmniNet.h>

#include <arpa/inet.h>
#include <array>
#include <netdb.h>
#include <sys/socket.h>
#include <openssl/asn1.h>
#include <openssl/evp.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

#include <chrono>
#include <cstring>
#include <ctime>
#include <iostream>
#include <sstream>
#include <string_view>

// §Phase 5.1: helper macro that converts LOG_CERR(INFO,  ...); style
// statements into LOG_INFO/LOG_WARNING/LOG_ERROR calls via the
// existing Logger. Same formatted text routed through the Logger
// instead of writing to stderr directly.
#define LOG_CERR(level, expr) do {                          \
        std::ostringstream _hive_msg;                   \
        _hive_msg << expr;                               \
        LOG_##level(_hive_msg.str());                    \
    } while (0)
WsRegistry& WsRegistry::instance() {
    static WsRegistry inst;
    return inst;
}

namespace {

// Lowercase a copy of a string (header names are case-insensitive).
std::string to_lower(std::string_view s) {
    std::string out(s);
    for (auto& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

// Trim leading/trailing whitespace.
std::string trim(std::string_view s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return std::string(s.substr(a, b - a));
}

// Parse a single header line "Name: value" into (lower-name, value).
// Returns nullopt on malformed input.
std::optional<std::pair<std::string,std::string>> parse_header_line(const std::string& line) {
    auto colon = line.find(':');
    if (colon == std::string::npos) return std::nullopt;
    std::string name = to_lower(trim(std::string_view(line).substr(0, colon)));
    std::string value = trim(std::string_view(line).substr(colon + 1));
    if (name.empty()) return std::nullopt;
    return std::make_pair(std::move(name), std::move(value));
}

}  // namespace

// ----- ParsedUrl -----

std::optional<HttpRequestTool::ParsedUrl>
HttpRequestTool::parse_url(const std::string& url) {
    if (url.size() < 8) return std::nullopt;  // shortest: http://h

    ParsedUrl out;
    size_t pos = 0;

    // scheme
    if (url.compare(0, 7, "http://") == 0) {
        out.scheme = "http";
        out.is_tls = false;
        out.port = 80;
        pos = 7;
    } else if (url.compare(0, 8, "https://") == 0) {
        out.scheme = "https";
        out.is_tls = true;
        out.port = 443;
        pos = 8;
    } else {
        return std::nullopt;  // only http/https
    }

    // host[:port][/path]
    size_t host_start = pos;
    size_t path_start = url.find('/', pos);
    size_t host_end = (path_start == std::string::npos) ? url.size() : path_start;

    std::string host_port = url.substr(host_start, host_end - host_start);
    if (host_port.empty()) return std::nullopt;

    // userinfo "user:pass@" — refuse to avoid sending credentials
    if (host_port.find('@') != std::string::npos) return std::nullopt;

    auto colon = host_port.find(':');
    if (colon != std::string::npos) {
        out.host = host_port.substr(0, colon);
        try {
            int p = std::stoi(host_port.substr(colon + 1));
            if (p < 1 || p > 65535) return std::nullopt;
            out.port = static_cast<uint16_t>(p);
        } catch (...) {
            return std::nullopt;
        }
    } else {
        out.host = host_port;
    }
    if (out.host.empty()) return std::nullopt;

    // path (may include query string)
    if (path_start == std::string::npos) {
        out.path = "/";
    } else {
        out.path = url.substr(path_start);
        if (out.path.empty()) out.path = "/";
    }

    return out;
}

// ----- do_request -----

json HttpRequestTool::do_request(const ParsedUrl& url,
                                 const std::string& method,
                                 const std::map<std::string,std::string>& headers,
                                 const std::string& body,
                                 int timeout_seconds,
                                 size_t max_response_bytes,
                                 bool insecure,
                                 ToolProgressFn progress) {
    using clock = std::chrono::steady_clock;
    auto t0 = clock::now();

    auto fail = [&](const std::string& msg) {
        return json::object({
            {"status", "error"},
            {"message", msg},
            {"final_url", url.scheme + "://" + url.host + ":" + std::to_string(url.port) + url.path},
            {"elapsed_ms", std::chrono::duration_cast<std::chrono::milliseconds>(clock::now() - t0).count()}
        });
    };

    // §Phase C2.5: byte-level progress during the body read.
    // Emits at fixed 25/50/75/100% milestones for Content-Length
    // responses (max 4 emissions). For chunked responses (no known
    // total) emits just "receiving" 0% at start and 100% at end.
    // The "last_pct" guard prevents flooding when chunks are small
    // (e.g. reading 1MB in 8KB chunks would otherwise emit thousands
    // of events). Initialised to -1 so the first 0% emit is not
    // filtered out by the `<= last_pct` check.
    int last_pct = -1;
    auto emit_progress = [&](int percent, const std::string& message) {
        if (!progress) return;
        if (percent <= last_pct && percent < 100) return;   // only fire on milestone crossings
        last_pct = percent;
        json p = {
            {"stage",   "receiving"},
            {"percent", percent},
            {"message", message},
        };
        progress(p);
    };
    auto maybe_emit_cl = [&](size_t bytes_read, size_t total) {
        if (total == 0) return;
        const int pct = static_cast<int>((bytes_read * 100) / total);
        // Milestone boundary at 25/50/75 — last_pct guard stops
        // repeated emissions if percent jumps multiple boundaries
        // in one chunk (e.g. a 1MB chunk on a 100KB total).
        int milestone = (pct / 25) * 25;
        if (milestone > last_pct) {
            emit_progress(milestone,
                          "Received " + std::to_string(bytes_read) +
                          " / " + std::to_string(total) + " bytes");
        }
    };

    // ----- Connect -----
    std::unique_ptr<OmniNet::Socket> sock;
    try {
        if (url.is_tls) {
            // OmniNet 0.7.2+ provides a proper client TLS API. As of
            // 0.7.4 the double-free regression in createSocket() is
            // also fixed, so we can use the clean factory pattern.
            //
            //   createClient()  - builds SSL_CTX, loads system CA
            //                    bundle, sets SSL_VERIFY_PEER
            //   createSocket()  - returns TLSSocket sharing the ctx
            auto tls_ctx = OmniNet::TLSContext::createClient();
            auto tls_sock = tls_ctx->createSocket();
            if (timeout_seconds > 0) {
                tls_sock->setRecvTimeout(std::chrono::seconds(timeout_seconds));
                tls_sock->setSendTimeout(std::chrono::seconds(timeout_seconds));
            }
            tls_sock->setSNIHostname(url.host);
            // Force HTTP/1.1 via ALPN. Many servers prefer h2 by
            // default and h2's wire format is incompatible with our
            // text-line parser. Requesting only http/1.1 makes the
            // server fall back to HTTP/1.1.
            tls_sock->setALPNProtocols({"http/1.1"});
            // insecure=true explicitly disables cert verification.
            // Otherwise we use the secure default (system CA + PEER).
            if (insecure) {
                tls_sock->setVerifyMode(OmniNet::TLSVerify::NONE);
                LOG_CERR(WARNING,  "[WARN] http_request: TLS verification disabled for "
                             << url.host);
            }
            tls_sock->connect(url.host, url.port);
            sock = std::move(tls_sock);
        } else {
            sock = OmniNet::createConnectedSocket(
                url.host, url.port,
                timeout_seconds > 0 ? std::chrono::seconds(timeout_seconds)
                                     : std::chrono::seconds(30));
        }
    } catch (const std::exception& e) {
        return fail(std::string("connect failed: ") + e.what());
    }

    // ----- Build request -----
    std::ostringstream req;
    req << method << " " << url.path << " HTTP/1.1\r\n";
    req << "Host: " << url.host << ":" << url.port << "\r\n";
    req << "User-Agent: hive-agent (OmniNet)\r\n";
    req << "Accept: */*\r\n";
    req << "Connection: close\r\n";
    if (!body.empty()) {
        req << "Content-Length: " << body.size() << "\r\n";
    }
    for (const auto& [k, v] : headers) {
        if (to_lower(k) == "host") continue;  // set above
        req << k << ": " << v << "\r\n";
    }
    req << "\r\n";
    if (!body.empty()) {
        req << body;
    }

    // ----- Send -----
    try {
        std::string request = req.str();
        sock->sendAll(std::span<const uint8_t>(
            reinterpret_cast<const uint8_t*>(request.data()), request.size()));
    } catch (const std::exception& e) {
        return fail(std::string("send failed: ") + e.what());
    }

    // ----- Read status line -----
    auto status_line = sock->readLine();
    if (!status_line) return fail("connection closed before status line");

    // Parse "HTTP/<major>.<minor> <code> <reason>"
    int status_code = 0;
    std::string status_text;
    {
        auto sp1 = status_line->find(' ');
        if (sp1 == std::string::npos) return fail("malformed status line");
        auto sp2 = status_line->find(' ', sp1 + 1);
        try {
            status_code = std::stoi(status_line->substr(sp1 + 1,
                                (sp2 == std::string::npos ? status_line->size() : sp2) - sp1 - 1));
        } catch (...) {
            return fail("malformed status code");
        }
        if (sp2 != std::string::npos) {
            status_text = trim(std::string_view(*status_line).substr(sp2 + 1));
        }
    }

    // ----- Read headers -----
    std::map<std::string,std::string> resp_headers;
    size_t content_length = 0;
    bool chunked = false;
    while (true) {
        auto line = sock->readLine();
        if (!line) return fail("connection closed mid-headers");
        if (line->empty()) break;  // empty line = end of headers
        auto kv = parse_header_line(*line);
        if (!kv) continue;  // skip malformed
        auto& [k, v] = *kv;
        if (k == "content-length") {
            try { content_length = std::stoul(v); } catch (...) {}
        } else if (k == "transfer-encoding") {
            chunked = (v.find("chunked") != std::string::npos);
        }
        resp_headers[k] = v;
    }

    // ----- Read body -----
    std::string body_out;
    bool truncated = false;

    // §FIX-head-no-body: HEAD responses carry Content-Length but no
    // body, and 204/304 never have one either — recvExactly() on them
    // would block until timeout and throw.
    const bool no_body = (method == "HEAD" ||
                          status_code == 204 || status_code == 304);

    if (!no_body && chunked) {
        // §Phase C2.5: chunked transfer has no Content-Length, so
        // we can't compute byte-level percent. Emit just "start" +
        // "end" — the user knows the response is in flight.
        emit_progress(0, "Receiving chunked body");
        // Minimal chunked transfer parser.
        while (true) {
            auto line = sock->readLine();
            if (!line) return fail("connection closed mid-chunk");
            std::string size_str = trim(*line);
            if (size_str.empty()) continue;
            // strip chunk extensions: e.g. "5;foo=bar" -> "5"
            auto semi = size_str.find(';');
            if (semi != std::string::npos) size_str = size_str.substr(0, semi);
            long long chunk_size = 0;
            try { chunk_size = std::stoll(size_str, nullptr, 16); } catch (...) { continue; }
            if (chunk_size == 0) {
                // consume trailing CRLF after final 0-chunk
                auto _ = sock->readLine();
                break;
            }
            if (body_out.size() + static_cast<size_t>(chunk_size) > max_response_bytes) {
                truncated = true;
                size_t allowed = max_response_bytes - body_out.size();
                if (allowed > 0) {
                    auto buf = sock->recvExactly(allowed);
                    body_out.append(reinterpret_cast<const char*>(buf.data()), buf.size());
                }
                // drain the rest of this chunk + trailing CRLF
                size_t drain = static_cast<size_t>(chunk_size) - allowed + 2;
                try { (void)sock->recvExactly(drain); } catch (...) {}
                break;
            }
            auto buf = sock->recvExactly(static_cast<size_t>(chunk_size));
            body_out.append(reinterpret_cast<const char*>(buf.data()), buf.size());
            // consume trailing CRLF
            (void)sock->readLine();
        }
        emit_progress(100, "Received " + std::to_string(body_out.size()) +
                          " bytes (chunked)");
    } else if (!no_body && content_length > 0) {
        // §Phase C2.5: we know the total up front, so emit 0% before
        // the read and milestone updates as bytes arrive. For small
        // responses (<25% of nothing) the only emission is 0% then
        // 100% — same as chunked. For >=25KB responses we get the
        // 4-milestone ladder.
        emit_progress(0, "Receiving " + std::to_string(content_length) + " bytes");
        if (content_length > max_response_bytes) {
            truncated = true;
            auto buf = sock->recvExactly(max_response_bytes);
            body_out.assign(reinterpret_cast<const char*>(buf.data()), buf.size());
            maybe_emit_cl(body_out.size(), content_length);
            // Connection: close — server will close when body sent; nothing to drain.
        } else {
            // For smaller-than-recv-buffer reads, recvExactly gives us
            // it in one shot — emit just the final 100%.
            auto buf = sock->recvExactly(content_length);
            body_out.assign(reinterpret_cast<const char*>(buf.data()), buf.size());
            emit_progress(100, "Received " + std::to_string(body_out.size()) +
                              " / " + std::to_string(content_length) + " bytes");
        }
    }
    // (Connection: close means we're done after body; no need to read more.)

    auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(clock::now() - t0).count();
    return json::object({
        {"status", "success"},
        {"status_code", status_code},
        {"status_text", status_text},
        {"headers", resp_headers},
        {"body", body_out},
        {"body_truncated", truncated},
        {"body_bytes", body_out.size()},
        {"final_url", url.scheme + "://" + url.host + ":" + std::to_string(url.port) + url.path},
        {"elapsed_ms", elapsed_ms}
    });
}

// ----- BaseTool -----

std::string HttpRequestTool::get_description() const {
    return "Make an HTTP/HTTPS request using OmniNet (TLS, SNI, configurable timeouts). "
           "Returns status, headers, and body. Use this instead of Bash `curl` "
           "or `python3 -c \"import urllib\"` for one-shot REST calls.";
}

json HttpRequestTool::get_definition() const {
    return json::object({
        {"type", "function"},
        {"function", json::object({
            {"name", get_name()},
            {"description", get_description()},
            {"parameters", json::object({
                {"type", "object"},
                {"properties", json::object({
                    {"url", json::object({
                        {"type", "string"},
                        {"description", "Absolute URL. Only http:// and https:// schemes are accepted. "
                                         "Format: scheme://host[:port][/path][?query]"}
                    })},
                    {"method", json::object({
                        {"type", "string"},
                        {"description", "HTTP method: GET, POST, PUT, DELETE, PATCH, HEAD, OPTIONS. "
                                         "Default: GET"},
                        {"enum", json::array({"GET","POST","PUT","DELETE","PATCH","HEAD","OPTIONS"})}
                    })},
                    {"headers", json::object({
                        {"type", "object"},
                        {"description", "Additional request headers as key/value pairs (strings). "
                                         "Common: {\"Content-Type\": \"application/json\"}. The Host header is set automatically."}
                    })},
                    {"body", json::object({
                        {"type", "string"},
                        {"description", "Request body (string). For JSON, also set Content-Type via headers."}
                    })},
                    {"timeout_seconds", json::object({
                        {"type", "integer"},
                        {"description", "Per-request timeout in seconds for connect+send+recv. "
                                         "Default 30, max 300."},
                        {"minimum", 1},
                        {"maximum", 300}
                    })},
                    {"insecure", json::object({
                        {"type", "boolean"},
                        {"description", "Skip TLS certificate verification. Off by default. "
                                         "Emits a [WARN] to stderr when true. Use only for self-signed test servers."}
                    })},
                    {"max_response_bytes", json::object({
                        {"type", "integer"},
                        {"description", "Cap on response body size in bytes. Larger responses are truncated. "
                                         "Default 5 MB (5242880), max 50 MB (52428800)."}
                    })}
                })},
                {"required", json::array({"url"})}
            })}
        })}
    });
}

json HttpRequestTool::execute(const json& args, FileIO& file_io) {
    return execute(args, file_io, nullptr);
}

json HttpRequestTool::execute(const json& args, FileIO& file_io,
                              ToolProgressFn progress) {
    (void)file_io;  // HTTP doesn't touch the local filesystem

    auto emit_progress = [&progress](const std::string& stage,
                                     int percent,
                                     const std::string& message) {
        if (!progress) return;
        json p = {
            {"stage",   stage},
            {"percent", percent},
            {"message", message},
        };
        progress(p);
    };

    // ----- Parse args -----
    std::string url, method = "GET", body;
    std::map<std::string,std::string> headers;
    int timeout_seconds = 30;
    bool insecure = false;
    size_t max_response_bytes = 5 * 1024 * 1024;  // 5 MB

    try {
        if (!args.is_object()) {
            return json::object({{"status", "error"}, {"message", "arguments must be a JSON object"}});
        }
        url = args.value("url", "");
        method = args.value("method", method);
        for (auto& c : method) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));

        if (args.contains("headers") && args["headers"].is_object()) {
            for (auto it = args["headers"].begin(); it != args["headers"].end(); ++it) {
                if (it.value().is_string()) {
                    headers[it.key()] = it.value().get<std::string>();
                }
            }
        }
        if (args.contains("body") && args["body"].is_string()) {
            body = args["body"].get<std::string>();
        }
        if (args.contains("timeout_seconds")) {
            timeout_seconds = args["timeout_seconds"].get<int>();
            if (timeout_seconds < 1)  timeout_seconds = 1;
            if (timeout_seconds > 300) timeout_seconds = 300;
        }
        if (args.contains("insecure") && args["insecure"].is_boolean()) {
            insecure = args["insecure"].get<bool>();
        }
        if (args.contains("max_response_bytes")) {
            long long n = args["max_response_bytes"].get<long long>();
            if (n < 1024)            n = 1024;
            if (n > 50 * 1024 * 1024) n = 50 * 1024 * 1024;
            max_response_bytes = static_cast<size_t>(n);
        }
    } catch (const std::exception& e) {
        return json::object({{"status", "error"}, {"message", std::string("argument parse error: ") + e.what()}});
    }

    if (url.empty()) {
        return json::object({{"status", "error"}, {"message", "missing required argument 'url'"}});
    }

    auto parsed = parse_url(url);
    if (!parsed) {
        return json::object({
            {"status", "error"},
            {"message", "invalid URL — only http:// and https:// are accepted; no userinfo"},
            {"url", url}
        });
    }

    // §Phase C2: emit "connecting" before the network round-trip. The
    // vast majority of latency for a typical http_request is the
    // connect+TLS handshake, so this is the stage the user wants to
    // see. Progress is only emitted AFTER all guards pass (URL parse,
    // arg validation) — matches BashTool's pattern of never emitting
    // when the tool is about to short-circuit return an error.
    emit_progress("connecting", 0, "Connecting to " + url);
    // §Phase C2.5: do_request now emits "receiving" with byte-level
    // percent (0/25/50/75/100% for Content-Length, just 0/100% for
    // chunked). The post-receive emit at the call-site is therefore
    // removed — do_request owns the receiving-stage events.
    json result = do_request(*parsed, method, headers, body,
                             timeout_seconds, max_response_bytes, insecure,
                             progress);
    return result;
}
// ============================================================================
// DnsLookupTool
// ============================================================================

std::string DnsLookupTool::get_description() const {
    return "Resolve a hostname to IP addresses (A and AAAA records). "
           "Uses the system resolver (getaddrinfo), so /etc/hosts, "
           "/etc/resolv.conf, and NSS configuration are honored. "
           "Useful for verifying DNS before http_request.";
}

json DnsLookupTool::get_definition() const {
    return json::object({
        {"type", "function"},
        {"function", json::object({
            {"name", get_name()},
            {"description", get_description()},
            {"parameters", json::object({
                {"type", "object"},
                {"properties", json::object({
                    {"hostname", json::object({
                        {"type", "string"},
                        {"description", "Hostname to resolve (e.g. \"api.github.com\"). "
                                         "Do NOT include scheme or path."}
                    })},
                    {"record_type", json::object({
                        {"type", "string"},
                        {"description", "Which records to return: \"A\" (IPv4 only, default), "
                                         "\"AAAA\" (IPv6 only), or \"ANY\" (both)."},
                        {"enum", json::array({"A", "AAAA", "ANY"})}
                    })}
                })},
                {"required", json::array({"hostname"})}
            })}
        })}
    });
}

json DnsLookupTool::execute(const json& args, FileIO& file_io) {
    (void)file_io;

    std::string hostname;
    std::string record_type = "A";
    if (!args.is_object()) {
        return json::object({{"status", "error"}, {"message", "arguments must be a JSON object"}});
    }
    try {
        hostname = args.value("hostname", "");
        record_type = args.value("record_type", record_type);
        for (auto& c : record_type) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    } catch (const std::exception& e) {
        return json::object({{"status", "error"}, {"message", std::string("argument parse error: ") + e.what()}});
    }

    if (hostname.empty()) {
        return json::object({{"status", "error"}, {"message", "missing required argument 'hostname'"}});
    }
    if (record_type != "A" && record_type != "AAAA" && record_type != "ANY") {
        return json::object({{"status", "error"},
                             {"message", "record_type must be A, AAAA, or ANY"}});
    }

    int ai_family = AF_UNSPEC;
    if (record_type == "A")   ai_family = AF_INET;
    if (record_type == "AAAA") ai_family = AF_INET6;

    addrinfo hints{};
    hints.ai_family = ai_family;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_ADDRCONFIG;

    addrinfo* res = nullptr;
    int rc = ::getaddrinfo(hostname.c_str(), nullptr, &hints, &res);
    if (rc != 0) {
        return json::object({
            {"status", "error"},
            {"message", std::string("getaddrinfo failed: ") + ::gai_strerror(rc)},
            {"hostname", hostname},
            {"record_type", record_type}
        });
    }

    json records = json::array();
    int v4_count = 0, v6_count = 0;
    for (addrinfo* p = res; p != nullptr; p = p->ai_next) {
        if (p->ai_family != AF_INET && p->ai_family != AF_INET6) continue;
        char ip_str[INET6_ADDRSTRLEN] = {0};
        if (p->ai_family == AF_INET) {
            inet_ntop(AF_INET, &reinterpret_cast<sockaddr_in*>(p->ai_addr)->sin_addr,
                      ip_str, sizeof(ip_str));
            ++v4_count;
        } else {
            inet_ntop(AF_INET6, &reinterpret_cast<sockaddr_in6*>(p->ai_addr)->sin6_addr,
                      ip_str, sizeof(ip_str));
            ++v6_count;
        }
        std::string canon = (p->ai_canonname ? p->ai_canonname : "");
        records.push_back(json::object({
            {"family", p->ai_family == AF_INET ? "IPv4" : "IPv6"},
            {"address", ip_str},
            {"canonical_name", canon}
        }));
    }
    ::freeaddrinfo(res);

    return json::object({
        {"status", "success"},
        {"hostname", hostname},
        {"record_type", record_type},
        {"records", records},
        {"count", records.size()},
        {"v4_count", v4_count},
        {"v6_count", v6_count}
    });
}

// ============================================================================
// TcpCheckTool
// ============================================================================

std::string TcpCheckTool::get_description() const {
    return "Test TCP reachability of host:port. Returns immediately on "
           "connect success or after the timeout. Use this before "
           "http_request to verify a service is up.";
}

json TcpCheckTool::get_definition() const {
    return json::object({
        {"type", "function"},
        {"function", json::object({
            {"name", get_name()},
            {"description", get_description()},
            {"parameters", json::object({
                {"type", "object"},
                {"properties", json::object({
                    {"host", json::object({
                        {"type", "string"},
                        {"description", "Hostname or IP address"}
                    })},
                    {"port", json::object({
                        {"type", "integer"},
                        {"description", "TCP port (1-65535)"},
                        {"minimum", 1},
                        {"maximum", 65535}
                    })},
                    {"timeout_seconds", json::object({
                        {"type", "integer"},
                        {"description", "Connect timeout. Default 5, max 60."},
                        {"minimum", 1},
                        {"maximum", 60}
                    })}
                })},
                {"required", json::array({"host", "port"})}
            })}
        })}
    });
}

json TcpCheckTool::execute(const json& args, FileIO& file_io) {
    (void)file_io;

    std::string host;
    int port = 0;
    int timeout_seconds = 5;
    if (!args.is_object()) {
        return json::object({{"status", "error"}, {"message", "arguments must be a JSON object"}});
    }
    try {
        host = args.value("host", "");
        port = args.value("port", 0);
        if (args.contains("timeout_seconds")) {
            timeout_seconds = args["timeout_seconds"].get<int>();
            if (timeout_seconds < 1)  timeout_seconds = 1;
            if (timeout_seconds > 60) timeout_seconds = 60;
        }
    } catch (const std::exception& e) {
        return json::object({{"status", "error"}, {"message", std::string("argument parse error: ") + e.what()}});
    }
    if (host.empty()) {
        return json::object({{"status", "error"}, {"message", "missing required argument 'host'"}});
    }
    if (port < 1 || port > 65535) {
        return json::object({{"status", "error"},
                             {"message", "port must be between 1 and 65535"},
                             {"port", port}});
    }

    using clock = std::chrono::steady_clock;
    auto t0 = clock::now();
    std::unique_ptr<OmniNet::Socket> sock;
    try {
        sock = OmniNet::createConnectedSocket(
            host, static_cast<uint16_t>(port),
            std::chrono::seconds(timeout_seconds));
    } catch (const std::exception& e) {
        auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(clock::now() - t0).count();
        return json::object({
            {"status", "error"},
            {"reachable", false},
            {"host", host},
            {"port", port},
            {"message", std::string("connect failed: ") + e.what()},
            {"elapsed_ms", elapsed_ms}
        });
    }
    auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(clock::now() - t0).count();
    return json::object({
        {"status", "success"},
        {"reachable", true},
        {"host", host},
        {"port", port},
        {"elapsed_ms", elapsed_ms}
    });
}
// ============================================================================
// TlsInspectTool
// ============================================================================

std::string TlsInspectTool::get_description() const {
    return "Inspect a TLS server: connect, dump cert chain, expiry, cipher. "
           "Returns negotiated TLS version, cipher suite, certificate "
           "subject/issuer/validity, signature algorithm, key algorithm + "
           "size, subjectAltNames, SHA-256 fingerprint, and days until "
           "expiry. No HTTP request is sent — the handshake alone gives all "
           "this info.";
}

namespace {

// RAII wrapper for X509*. Calls X509_free on destruction, taking
// ownership of the pointer returned by SSL_get_peer_certificate().
// Move-only so accidental copies don't double-free.
struct X509Holder {
    X509* p = nullptr;
    X509Holder() = default;
    explicit X509Holder(X509* x) : p(x) {}
    ~X509Holder() { if (p) X509_free(p); }
    X509Holder(const X509Holder&) = delete;
    X509Holder& operator=(const X509Holder&) = delete;
    X509Holder(X509Holder&& o) noexcept : p(o.p) { o.p = nullptr; }
    X509Holder& operator=(X509Holder&& o) noexcept {
        if (this != &o) { if (p) X509_free(p); p = o.p; o.p = nullptr; }
        return *this;
    }
    explicit operator bool() const { return p != nullptr; }
    X509* get() const { return p; }
};

// §FIX-pkey-leak: same RAII pattern as X509Holder, for the EVP_PKEY*
// returned by X509_get_pubkey() — previously passed to describe_pkey()
// and never freed.
struct EvpPkeyHolder {
    EVP_PKEY* p = nullptr;
    EvpPkeyHolder() = default;
    explicit EvpPkeyHolder(EVP_PKEY* x) : p(x) {}
    ~EvpPkeyHolder() { if (p) EVP_PKEY_free(p); }
    EvpPkeyHolder(const EvpPkeyHolder&) = delete;
    EvpPkeyHolder& operator=(const EvpPkeyHolder&) = delete;
    EvpPkeyHolder(EvpPkeyHolder&& o) noexcept : p(o.p) { o.p = nullptr; }
    EvpPkeyHolder& operator=(EvpPkeyHolder&& o) noexcept {
        if (this != &o) { if (p) EVP_PKEY_free(p); p = o.p; o.p = nullptr; }
        return *this;
    }
    explicit operator bool() const { return p != nullptr; }
    EVP_PKEY* get() const { return p; }
};

// Convert ASN1_TIME (e.g. "Jun 26 12:00:00 2026 GMT") to ISO 8601
// ("2026-06-26T12:00:00Z"). Returns empty string on parse failure.
std::string asn1_to_iso8601(const ASN1_TIME* t) {
    if (!t) return "";
    struct tm tm_buf{};
    if (ASN1_TIME_to_tm(t, &tm_buf) != 1) return "";
    // ASN1_TIME_to_tm returns tm in UTC. timegm converts struct tm (UTC) to time_t.
    time_t tt = timegm(&tm_buf);
    if (tt == -1) return "";
    char buf[32];
    if (std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm_buf) == 0) return "";
    return std::string(buf);
}

// Days between now and expiry. Negative if already expired.
long days_until_expiry(const ASN1_TIME* not_after) {
    if (!not_after) return 0;
    struct tm tm_buf{};
    if (ASN1_TIME_to_tm(not_after, &tm_buf) != 1) return 0;
    time_t tt = timegm(&tm_buf);
    if (tt == -1) return 0;
    time_t now = ::time(nullptr);
    return static_cast<long>((tt - now) / 86400);
}

// Pull X509_NAME into a short string. Uses X509_NAME_oneline which gives
// "/C=US/O=GitHub/CN=api.github.com" format — compact and machine-readable.
std::string name_to_string(X509_NAME* name) {
    if (!name) return "";
    char* buf = X509_NAME_oneline(name, nullptr, 0);
    if (!buf) return "";
    std::string out(buf);
    OPENSSL_free(buf);
    return out;
}

// Pull all subjectAltName entries as {type, value} pairs.
json extract_san(X509* cert) {
    json san = json::array();
    if (!cert) return san;
    GENERAL_NAMES* names = static_cast<GENERAL_NAMES*>(
        X509_get_ext_d2i(cert, NID_subject_alt_name, nullptr, nullptr));
    if (!names) return san;

    for (int i = 0; i < sk_GENERAL_NAME_num(names); ++i) {
        GENERAL_NAME* gn = sk_GENERAL_NAME_value(names, i);
        if (!gn) continue;
        if (gn->type == GEN_DNS) {
            const unsigned char* dns = ASN1_STRING_get0_data(gn->d.dNSName);
            int len = ASN1_STRING_length(gn->d.dNSName);
            if (dns && len > 0) {
                san.push_back(json::object({
                    {"type", "DNS"},
                    {"value", std::string(reinterpret_cast<const char*>(dns), len)}
                }));
            }
        } else if (gn->type == GEN_IPADD) {
            unsigned char buf[16];
            int len = (gn->d.iPAddress->length < 16) ? gn->d.iPAddress->length : 16;
            std::memcpy(buf, gn->d.iPAddress->data, len);
            char ip_str[INET6_ADDRSTRLEN] = {0};
            int af = (len == 4) ? AF_INET : AF_INET6;
            inet_ntop(af, buf, ip_str, sizeof(ip_str));
            san.push_back(json::object({
                {"type", af == AF_INET ? "IP" : "IPv6"},
                {"value", ip_str}
            }));
        }
    }
    GENERAL_NAMES_free(names);
    return san;
}

// Convert EVP_PKEY to {algorithm, size_bits} JSON.
json describe_pkey(EVP_PKEY* pkey) {
    json out = json::object();
    if (!pkey) return out;
    int id = EVP_PKEY_base_id(pkey);
    switch (id) {
        case EVP_PKEY_RSA: out["algorithm"] = "RSA"; break;
        case EVP_PKEY_EC:  out["algorithm"] = "EC";  break;
        case EVP_PKEY_ED25519: out["algorithm"] = "Ed25519"; break;
        case EVP_PKEY_ED448:   out["algorithm"] = "Ed448"; break;
        default: out["algorithm"] = std::string("unknown(") + std::to_string(id) + ")"; break;
    }
    if (id == EVP_PKEY_RSA || id == EVP_PKEY_EC) {
        out["size_bits"] = EVP_PKEY_bits(pkey);
    }
    return out;
}

}  // namespace

json TlsInspectTool::get_definition() const {
    return json::object({
        {"type", "function"},
        {"function", json::object({
            {"name", get_name()},
            {"description", get_description()},
            {"parameters", json::object({
                {"type", "object"},
                {"properties", json::object({
                    {"host", json::object({
                        {"type", "string"},
                        {"description", "Hostname or IP address"}
                    })},
                    {"port", json::object({
                        {"type", "integer"},
                        {"description", "TCP port (default 443)."},
                        {"default", 443},
                        {"minimum", 1},
                        {"maximum", 65535}
                    })},
                    {"timeout_seconds", json::object({
                        {"type", "integer"},
                        {"description", "Connect timeout. Default 10, max 60."},
                        {"minimum", 1},
                        {"maximum", 60}
                    })},
                    {"insecure", json::object({
                        {"type", "boolean"},
                        {"description", "Skip TLS cert verification. Off by default."}
                    })}
                })},
                {"required", json::array({"host"})}
            })}
        })}
    });
}

json TlsInspectTool::execute(const json& args, FileIO& file_io) {
    (void)file_io;

    std::string host;
    int port = 443;
    int timeout_seconds = 10;
    bool insecure = false;
    if (!args.is_object()) {
        return json::object({{"status", "error"}, {"message", "arguments must be a JSON object"}});
    }
    try {
        host = args.value("host", "");
        port = args.value("port", port);
        if (args.contains("timeout_seconds")) {
            timeout_seconds = args["timeout_seconds"].get<int>();
            if (timeout_seconds < 1)  timeout_seconds = 1;
            if (timeout_seconds > 60) timeout_seconds = 60;
        }
        if (args.contains("insecure") && args["insecure"].is_boolean()) {
            insecure = args["insecure"].get<bool>();
        }
    } catch (const std::exception& e) {
        return json::object({{"status", "error"}, {"message", std::string("argument parse error: ") + e.what()}});
    }
    if (host.empty()) {
        return json::object({{"status", "error"}, {"message", "missing required argument 'host'"}});
    }
    if (port < 1 || port > 65535) {
        return json::object({{"status", "error"}, {"message", "port must be 1-65535"}});
    }

    using clock = std::chrono::steady_clock;
    auto t0 = clock::now();

    auto fail = [&](const std::string& msg) {
        return json::object({
            {"status", "error"},
            {"host", host},
            {"port", port},
            {"message", msg},
            {"elapsed_ms", std::chrono::duration_cast<std::chrono::milliseconds>(clock::now() - t0).count()}
        });
    };

    std::unique_ptr<OmniNet::TLSSocket> tls_sock;
    try {
        auto tls_ctx = OmniNet::TLSContext::createClient();
        tls_sock.reset(tls_ctx->createSocket().release());
        tls_sock->setRecvTimeout(std::chrono::seconds(timeout_seconds));
        tls_sock->setSendTimeout(std::chrono::seconds(timeout_seconds));
        tls_sock->setSNIHostname(host);
        if (insecure) {
            tls_sock->setVerifyMode(OmniNet::TLSVerify::NONE);
            LOG_CERR(WARNING,  "[WARN] tls_inspect: TLS verification disabled for "
                      << host);
        }
        tls_sock->connect(host, static_cast<uint16_t>(port));
    } catch (const std::exception& e) {
        return fail(std::string("connect failed: ") + e.what());
    }

    json cert_json = json::object();
    SSL* ssl = tls_sock->getSSL();
    // RAII: cert is freed when holder goes out of scope (including on
    // exception paths). Previously a manual X509_free(cert) call at the
    // end meant any exception in between leaked the cert.
    X509Holder cert(ssl ? SSL_get_peer_certificate(ssl) : nullptr);
    if (!cert) {
        cert_json["present"] = false;
    } else try {
        cert_json["present"] = true;
        cert_json["subject"] = name_to_string(X509_get_subject_name(cert.p));
        cert_json["issuer"]  = name_to_string(X509_get_issuer_name(cert.p));
        cert_json["not_before"] = asn1_to_iso8601(X509_get_notBefore(cert.p));
        cert_json["not_after"]  = asn1_to_iso8601(X509_get_notAfter(cert.p));
        cert_json["days_until_expiry"] = days_until_expiry(X509_get_notAfter(cert.p));

        int sig_nid = X509_get_signature_nid(cert.p);
        const char* sig_name = OBJ_nid2sn(sig_nid);
        cert_json["signature_algorithm"] = sig_name ? sig_name : "unknown";

        EvpPkeyHolder pkey(X509_get_pubkey(cert.p));
        cert_json["public_key"] = describe_pkey(pkey.p);
        cert_json["subject_alt_names"] = extract_san(cert.p);

        // Compute SHA-256 fingerprint directly from our held cert so
        // we don't rely on OmniNet's getPeerCertificateFingerprint()
        // (which would re-parse the cert internally).
        std::array<uint8_t, 32> md{};
        unsigned int len = 0;
        if (X509_digest(cert.p, EVP_sha256(), md.data(), &len) == 1 && len == 32) {
            std::string fp;
            fp.reserve(64);
            for (unsigned int i = 0; i < len; ++i) {
                static const char hex[] = "0123456789ABCDEF";
                fp.push_back(hex[(md[i] >> 4) & 0xF]);
                fp.push_back(hex[md[i] & 0xF]);
            }
            cert_json["fingerprint_sha256"] = fp;
        }
    } catch (const std::exception& e) {
        // cert freed automatically by X509Holder destructor on return
        return fail(std::string("cert extraction failed: ") + e.what());
    }

    std::string version = tls_sock->getVersion();
    std::string cipher = tls_sock->getCipher();
    // Real verification result: OpenSSL returns X509_V_OK (0) if the chain
    // was validated against the configured trust store. hasPeerCertificate()
    // just reports that the peer sent *a* cert, not whether we trust it.
            long verify_result = ssl ? SSL_get_verify_result(ssl) : X509_V_ERR_UNSPECIFIED;
    bool verified = (verify_result == X509_V_OK) && !insecure;
    std::string verify_message = X509_verify_cert_error_string(verify_result);

    auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(clock::now() - t0).count();
    return json::object({
        {"status", "success"},
        {"host", host},
        {"port", port},
        {"elapsed_ms", elapsed_ms},
        {"tls_version", version},
        {"cipher", cipher},
        {"alpn_protocol", tls_sock->getALPNProtocol().value_or("")},
        {"peer_cert_verified", verified},
        {"verify_result", verify_message},
        {"insecure_requested", insecure},
        {"certificate", cert_json}
    });
}// ============================================================================
// WsRegistry — singleton holding live WebSocket connections
// ============================================================================

// Forward-declare Connection so we can #include the WebSocket header here.
// NetworkTool.cpp is the only TU that includes OmniNet's WebSocket APIs;
// the rest of the engine sees only the forward declaration in the header.

#include "websocket/WebSocket.h"

#include <condition_variable>
#include <thread>


WsRegistry::WsRegistry() {
    start_reaper();
}

WsRegistry::~WsRegistry() {
    stop_reaper();
}

void WsRegistry::start_reaper() {
    if (reaper_running_.exchange(true)) return;
    reaper_ = std::thread([this] {
        using namespace std::chrono_literals;
        while (reaper_running_.load()) {
            // Wait for up to 30s or until shutdown is requested.
            {
                std::unique_lock lk(mu_);
                reaper_cv_.wait_for(lk, 30s,
                                    [this] { return !reaper_running_.load(); });
            }
            if (!reaper_running_.load()) break;

            // Sweep: drop connections idle > 5 min.
            auto now = std::chrono::steady_clock::now();
            std::lock_guard lock(mu_);
            for (auto it = conns_.begin(); it != conns_.end(); ) {
                auto& entry = *it->second;
                if (now - entry.last_used > 5min) {
                    if (entry.open.load() && entry.ws) {
                        try { entry.ws->close(); } catch (...) {}
                    }
                    it = conns_.erase(it);
                } else {
                    ++it;
                }
            }
        }
    });
}

void WsRegistry::stop_reaper() {
    if (!reaper_running_.exchange(false)) return;
    reaper_cv_.notify_all();
    if (reaper_.joinable()) reaper_.join();
}

std::optional<std::string> WsRegistry::connect(const std::string& url, std::string* err) {
    // Validate scheme
    if (url.compare(0, 5, "ws://") != 0 && url.compare(0, 6, "wss://") != 0) {
        if (err) *err = "only ws:// and wss:// schemes accepted";
        return std::nullopt;
    }
    // wss:// is supported by OmniNet 0.7.6+ (Bug 9 fix: the ctor now
    // calls m_socket->connect() before performClientHandshake() so
    // TLSSocket::recvAll no longer hits FD_SET(INVALID_SOCKET, ...)).
    // The handshake uses the system CA bundle + SSL_VERIFY_PEER.

    auto entry = std::make_shared<Entry>();   // shared_ptr — see Entry docs
    entry->url = url;
    entry->last_used = std::chrono::steady_clock::now();

    try {
        // OmniNet 0.7.4: WebSocket::Connection ctor takes (url, protocol).
        // The default TLS settings (system CA + VERIFY_PEER) apply for wss://.
        entry->ws = std::make_unique<OmniNet::WebSocket::Connection>(url);
    } catch (const std::exception& e) {
        if (err) *err = std::string("websocket connect failed: ") + e.what();
        return std::nullopt;
    }

    if (!entry->ws->isOpen()) {
        if (err) *err = "websocket handshake did not complete (connection not open)";
        return std::nullopt;
    }

    // Assign id
    uint64_t n = next_id_.fetch_add(1);
    char buf[32];
    std::snprintf(buf, sizeof(buf), "ws_0x%lx",
                  static_cast<unsigned long>(n));
    std::string id = buf;

    entry->open.store(entry->ws->isOpen());
    std::lock_guard lock(mu_);
    conns_.emplace(id, std::move(entry));
    return id;
}

// Returns shared_ptr so the caller holds a reference even if the reaper
// thread erases the entry from the map concurrently. Without this, the
// reaper could destroy the Entry while send/recv/close are still using
// it -> use-after-free.
std::shared_ptr<WsRegistry::Entry> WsRegistry::get(const std::string& id) {
    std::lock_guard lock(mu_);
    auto it = conns_.find(id);
    if (it == conns_.end()) return nullptr;
    it->second->last_used = std::chrono::steady_clock::now();
    return it->second;
}

std::vector<std::string> WsRegistry::list_ids() const {
    std::lock_guard lock(mu_);
    std::vector<std::string> out;
    out.reserve(conns_.size());
    for (auto& [id, _] : conns_) out.push_back(id);
    return out;
}

bool WsRegistry::close(const std::string& id, int code, const std::string& reason) {
    // shared_ptr so we keep the Entry alive while we send the Close
    // frame, even if the reaper concurrently erases the map entry.
    std::shared_ptr<Entry> e;
    {
        std::lock_guard lock(mu_);
        auto it = conns_.find(id);
        if (it == conns_.end()) return false;
        e = std::move(it->second);
        conns_.erase(it);
    }
    if (e && e->ws && e->open.load()) {
        try {
            // Map common integer codes to CloseCode enum; default to NORMAL.
            auto cc = OmniNet::WebSocket::CloseCode::NORMAL;
            if      (code == 1000) cc = OmniNet::WebSocket::CloseCode::NORMAL;
            else if (code == 1001) cc = OmniNet::WebSocket::CloseCode::GOING_AWAY;
            else if (code == 1002) cc = OmniNet::WebSocket::CloseCode::PROTOCOL_ERROR;
            else if (code == 1003) cc = OmniNet::WebSocket::CloseCode::UNSUPPORTED_DATA;
            else if (code == 1008) cc = OmniNet::WebSocket::CloseCode::POLICY_VIOLATION;
            else if (code == 1011) cc = OmniNet::WebSocket::CloseCode::INTERNAL_ERROR;
            e->ws->close(cc, reason);
        } catch (...) {
            // best-effort close
        }
        e->open.store(false);
    }
    return true;
}

size_t WsRegistry::size_unsafe() const {
    // Caller must hold the lock — we don't take it here to avoid recursion.
    // Used by tests only.
    return conns_.size();
}

// ============================================================================
// WebSocketConnectTool
// ============================================================================

std::string WebSocketConnectTool::get_description() const {
    return "Open a WebSocket connection to a ws:// or wss:// URL. Returns a "
           "connection_id that you pass back to websocket_send, "
           "websocket_recv, and websocket_close. Idle connections are "
           "automatically closed after 5 minutes of inactivity. "
           "wss:// URLs use TLS cert verification against the system CA.";
}

json WebSocketConnectTool::get_definition() const {
    return json::object({
        {"type", "function"},
        {"function", json::object({
            {"name", get_name()},
            {"description", get_description()},
            {"parameters", json::object({
                {"type", "object"},
                {"properties", json::object({
                    {"url", json::object({
                        {"type", "string"},
                        {"description", "WebSocket URL: ws://host[:port][/path] "
                                         "or wss://host[:port][/path]. "
                                         "Subprotocols can be appended via "
                                         "the 'protocol' field, not the URL."}
                    })},
                    {"protocol", json::object({
                        {"type", "string"},
                        {"description", "Optional Sec-WebSocket-Protocol value."}
                    })},
                    {"timeout_seconds", json::object({
                        {"type", "integer"},
                        {"description", "Connect + handshake timeout. Default 10, max 60."},
                        {"minimum", 1},
                        {"maximum", 60}
                    })}
                })},
                {"required", json::array({"url"})}
            })}
        })}
    });
}

json WebSocketConnectTool::execute(const json& args, FileIO& file_io) {
    (void)file_io;

    std::string url, protocol;
    int timeout_seconds = 10;
    if (!args.is_object()) {
        return json::object({{"status", "error"}, {"message", "arguments must be a JSON object"}});
    }
    try {
        url = args.value("url", "");
        protocol = args.value("protocol", "");
        if (args.contains("timeout_seconds")) {
            timeout_seconds = args["timeout_seconds"].get<int>();
            if (timeout_seconds < 1)  timeout_seconds = 1;
            if (timeout_seconds > 60) timeout_seconds = 60;
        }
    } catch (const std::exception& e) {
        return json::object({{"status", "error"}, {"message", std::string("argument parse error: ") + e.what()}});
    }
    if (url.empty()) {
        return json::object({{"status", "error"}, {"message", "missing required argument 'url'"}});
    }

    std::string err;
    auto id = WsRegistry::instance().connect(url, &err);
    if (!id) {
        return json::object({{"status", "error"}, {"message", err}, {"url", url}});
    }
    return json::object({
        {"status", "success"},
        {"connection_id", *id},
        {"url", url},
        {"protocol", protocol}
    });
}

// ============================================================================
// WebSocketSendTool
// ============================================================================

std::string WebSocketSendTool::get_description() const {
    return "Send a text or binary message on an open WebSocket connection. "
           "Use 'message' (string) for text frames, 'message_base64' for "
           "binary. Pass the connection_id returned by websocket_connect.";
}

json WebSocketSendTool::get_definition() const {
    return json::object({
        {"type", "function"},
        {"function", json::object({
            {"name", get_name()},
            {"description", get_description()},
            {"parameters", json::object({
                {"type", "object"},
                {"properties", json::object({
                    {"connection_id", json::object({
                        {"type", "string"},
                        {"description", "WebSocket connection id (from websocket_connect)."}
                    })},
                    {"message", json::object({
                        {"type", "string"},
                        {"description", "Text message to send (UTF-8). Mutually exclusive with "
                                         "message_base64."}
                    })},
                    {"message_base64", json::object({
                        {"type", "string"},
                        {"description", "Binary message, encoded as base64."}
                    })}
                })},
                {"required", json::array({"connection_id"})}
            })}
        })}
    });
}

namespace {

// Decode base64 to bytes. Returns empty optional on bad input.
std::optional<std::vector<uint8_t>> base64_decode(const std::string& in) {
    static const int8_t T[256] = {
        // 0..255; -1 = invalid, else 0..63
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,62,-1,-1,-1,63,
        52,53,54,55,56,57,58,59,60,61,-1,-1,-1,-1,-1,-1,
        -1, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9,10,11,12,13,14,
        15,16,17,18,19,20,21,22,23,24,25,-1,-1,-1,-1,-1,
        -1,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,
        41,42,43,44,45,46,47,48,49,50,51,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1
    };

    std::vector<uint8_t> out;
    out.reserve((in.size() * 3) / 4);
    int buf = 0, bits = 0;
    for (unsigned char c : in) {
        if (c == '=' || c == '\n' || c == '\r' || c == ' ') continue;
        int8_t v = T[c];
        if (v < 0) return std::nullopt;
        buf = (buf << 6) | v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<uint8_t>((buf >> bits) & 0xFF));
        }
    }
    return out;
}

}  // namespace

json WebSocketSendTool::execute(const json& args, FileIO& file_io) {
    (void)file_io;

    std::string conn_id, message;
    bool has_message = false, has_b64 = false;
    std::vector<uint8_t> bytes;
    if (!args.is_object()) {
        return json::object({{"status", "error"}, {"message", "arguments must be a JSON object"}});
    }
    try {
        conn_id = args.value("connection_id", "");
        if (args.contains("message")) {
            message = args["message"].get<std::string>();
            has_message = true;
        }
        if (args.contains("message_base64")) {
            std::string b64 = args["message_base64"].get<std::string>();
            auto dec = base64_decode(b64);
            if (!dec) return json::object({{"status", "error"}, {"message", "message_base64 is not valid base64"}});
            bytes = std::move(*dec);
            has_b64 = true;
        }
    } catch (const std::exception& e) {
        return json::object({{"status", "error"}, {"message", std::string("argument parse error: ") + e.what()}});
    }
    if (conn_id.empty()) {
        return json::object({{"status", "error"}, {"message", "missing required argument 'connection_id'"}});
    }
    if (has_message == has_b64) {
        return json::object({{"status", "error"},
                             {"message", "exactly one of 'message' or 'message_base64' is required"}});
    }

    auto entry = WsRegistry::instance().get(conn_id);
    if (!entry) {
        return json::object({{"status", "error"},
                             {"message", "unknown connection_id"},
                             {"connection_id", conn_id}});
    }
    if (!entry->open.load()) {
        return json::object({{"status", "error"},
                             {"message", "connection is closed"},
                             {"connection_id", conn_id}});
    }
    try {
        if (has_message) {
            entry->ws->sendText(message);
        } else {
            entry->ws->sendBinary(bytes);
        }
    } catch (const std::exception& e) {
        entry->open.store(false);
        return json::object({{"status", "error"},
                             {"message", std::string("send failed: ") + e.what()},
                             {"connection_id", conn_id}});
    }
    return json::object({
        {"status", "success"},
        {"connection_id", conn_id},
        {"bytes_sent", has_message ? message.size() : bytes.size()},
        {"type", has_message ? "text" : "binary"}
    });
}

// ============================================================================
// WebSocketRecvTool
// ============================================================================

std::string WebSocketRecvTool::get_description() const {
    return "Receive the next WebSocket frame on an open connection. Returns "
           "{type: \"text\"|\"binary\"|\"close\"|\"ping\"|\"pong\", payload}. "
           "Set 'timeout_seconds' to bound how long to wait. Text frames "
           "return the payload as a UTF-8 string. Binary frames return the "
           "payload as a base64-encoded string (since LLM context is text). "
           "A close frame signals the peer has closed the connection.";
}

json WebSocketRecvTool::get_definition() const {
    return json::object({
        {"type", "function"},
        {"function", json::object({
            {"name", get_name()},
            {"description", get_description()},
            {"parameters", json::object({
                {"type", "object"},
                {"properties", json::object({
                    {"connection_id", json::object({
                        {"type", "string"},
                        {"description", "WebSocket connection id."}
                    })},
                    {"timeout_seconds", json::object({
                        {"type", "integer"},
                        {"description", "How long to wait for a frame. Default 30, max 300."},
                        {"minimum", 1},
                        {"maximum", 300}
                    })}
                })},
                {"required", json::array({"connection_id"})}
            })}
        })}
    });
}

namespace {

// Base64-encode bytes to a string (RFC 4648). Used for binary payloads
// since the LLM tool channel is JSON text.
std::string base64_encode(std::span<const uint8_t> data) {
    static const char* tbl =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((data.size() + 2) / 3) * 4);
    size_t i = 0;
    while (i + 3 <= data.size()) {
        uint32_t v = (uint32_t(data[i]) << 16) | (uint32_t(data[i+1]) << 8) | data[i+2];
        out.push_back(tbl[(v >> 18) & 0x3F]);
        out.push_back(tbl[(v >> 12) & 0x3F]);
        out.push_back(tbl[(v >>  6) & 0x3F]);
        out.push_back(tbl[ v        & 0x3F]);
        i += 3;
    }
    if (i < data.size()) {
        uint32_t v = uint32_t(data[i]) << 16;
        if (i + 1 < data.size()) v |= uint32_t(data[i+1]) << 8;
        out.push_back(tbl[(v >> 18) & 0x3F]);
        out.push_back(tbl[(v >> 12) & 0x3F]);
        out.push_back((i + 1 < data.size()) ? tbl[(v >> 6) & 0x3F] : '=');
        out.push_back('=');
    }
    return out;
}

}  // namespace

json WebSocketRecvTool::execute(const json& args, FileIO& file_io) {
    (void)file_io;

    std::string conn_id;
    int timeout_seconds = 30;
    if (!args.is_object()) {
        return json::object({{"status", "error"}, {"message", "arguments must be a JSON object"}});
    }
    try {
        conn_id = args.value("connection_id", "");
        if (args.contains("timeout_seconds")) {
            timeout_seconds = args["timeout_seconds"].get<int>();
            if (timeout_seconds < 1)   timeout_seconds = 1;
            if (timeout_seconds > 300) timeout_seconds = 300;
        }
    } catch (const std::exception& e) {
        return json::object({{"status", "error"}, {"message", std::string("argument parse error: ") + e.what()}});
    }
    if (conn_id.empty()) {
        return json::object({{"status", "error"}, {"message", "missing required argument 'connection_id'"}});
    }

    auto entry = WsRegistry::instance().get(conn_id);
    if (!entry) {
        return json::object({{"status", "error"},
                             {"message", "unknown connection_id"},
                             {"connection_id", conn_id}});
    }
    if (!entry->open.load()) {
        return json::object({{"status", "error"},
                             {"message", "connection is closed"},
                             {"connection_id", conn_id}});
    }

    // Bound the wait by setting the underlying socket's recv timeout.
    if (entry->ws && entry->ws->m_socket) {
        entry->ws->m_socket->setRecvTimeout(std::chrono::seconds(timeout_seconds));
    }

    // OmniNet 0.7.5+ (commit cacca33) rewrote Connection::receive() to
    // buffer + decode properly (previously read only 2 bytes per
    // recv() and looped forever on multi-byte payloads). Use it
    // directly — no workaround needed.
    std::optional<OmniNet::WebSocket::Frame> frame;
    try {
        frame = entry->ws->receive();
    } catch (const std::exception& e) {
        entry->open.store(false);
        return json::object({{"status", "error"},
                             {"message", std::string("recv failed: ") + e.what()},
                             {"connection_id", conn_id}});
    }

    if (!frame) {
        entry->open.store(false);
        return json::object({
            {"status", "closed"},
            {"connection_id", conn_id},
            {"message", "peer closed the connection"}
        });
    }

    json out = {
        {"status", "success"},
        {"connection_id", conn_id}
    };

    if (frame->isText()) {
        out["type"] = "text";
        out["payload"] = std::string(frame->payload.begin(), frame->payload.end());
    } else if (frame->isBinary()) {
        out["type"] = "binary";
        out["payload_base64"] = base64_encode(frame->payload);
        out["bytes"] = frame->payload.size();
    } else if (frame->isClose()) {
        out["type"] = "close";
        out["payload_base64"] = base64_encode(frame->payload);
        out["bytes"] = frame->payload.size();
        entry->open.store(false);
    } else if (frame->isPing()) {
        out["type"] = "ping";
        out["payload_base64"] = base64_encode(frame->payload);
        out["bytes"] = frame->payload.size();
    } else if (frame->isPong()) {
        out["type"] = "pong";
        out["payload_base64"] = base64_encode(frame->payload);
        out["bytes"] = frame->payload.size();
    } else {
        out["type"] = "unknown";
        out["payload_base64"] = base64_encode(frame->payload);
        out["bytes"] = frame->payload.size();
    }
    return out;
}

// ============================================================================
// WebSocketCloseTool
// ============================================================================

std::string WebSocketCloseTool::get_description() const {
    return "Close an open WebSocket connection cleanly (sends a Close "
           "frame with the given status code and reason). Returns success "
           "even if the connection was already closed.";
}

json WebSocketCloseTool::get_definition() const {
    return json::object({
        {"type", "function"},
        {"function", json::object({
            {"name", get_name()},
            {"description", get_description()},
            {"parameters", json::object({
                {"type", "object"},
                {"properties", json::object({
                    {"connection_id", json::object({
                        {"type", "string"},
                        {"description", "WebSocket connection id."}
                    })},
                    {"code", json::object({
                        {"type", "integer"},
                        {"description", "Close status code (default 1000 = normal closure). "
                                         "Common values: 1000 normal, 1001 going away, "
                                         "1002 protocol error, 1003 unsupported data, "
                                         "1008 policy violation, 1011 internal error."},
                        {"minimum", 1000},
                        {"maximum", 4999}
                    })},
                    {"reason", json::object({
                        {"type", "string"},
                        {"description", "Optional human-readable close reason."}
                    })}
                })},
                {"required", json::array({"connection_id"})}
            })}
        })}
    });
}

json WebSocketCloseTool::execute(const json& args, FileIO& file_io) {
    (void)file_io;

    std::string conn_id, reason;
    int code = 1000;
    if (!args.is_object()) {
        return json::object({{"status", "error"}, {"message", "arguments must be a JSON object"}});
    }
    try {
        conn_id = args.value("connection_id", "");
        code = args.value("code", code);
        reason = args.value("reason", reason);
    } catch (const std::exception& e) {
        return json::object({{"status", "error"}, {"message", std::string("argument parse error: ") + e.what()}});
    }
    if (conn_id.empty()) {
        return json::object({{"status", "error"}, {"message", "missing required argument 'connection_id'"}});
    }
    if (code < 1000 || code > 4999) {
        return json::object({{"status", "error"}, {"message", "code must be in [1000, 4999]"}, {"code", code}});
    }

    bool existed = WsRegistry::instance().close(conn_id, code, reason);
    if (!existed) {
        return json::object({{"status", "error"},
                             {"message", "unknown connection_id"},
                             {"connection_id", conn_id}});
    }
    return json::object({
        {"status", "success"},
        {"connection_id", conn_id},
        {"code", code},
        {"reason", reason}
    });
}
