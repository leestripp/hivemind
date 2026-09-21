#pragma once

#include <expected>
#include <string>
#include <utility>
#include <vector>

// Synchronous HTTP(S) GET for the web_fetch agent tool, built on OmniNet's
// HttpClient (the project's HTTP stack — no separate libcurl usage here).
// Ported from MonkeyByte (monkeybyte/backend/WebFetch) 2026-09-09.
class WebFetch {
public:
    struct Result {
        long status = 0;
        std::string body;
        std::string content_type; // lowercase, parameters stripped
    };

    static constexpr std::size_t kMaxBody = 2 * 1024 * 1024; // 2 MB

    // Follows redirects, 10 s total timeout, body capped at kMaxBody.
    // Only http:// and https:// URLs are accepted; non-2xx statuses are
    // errors.
    static std::expected<Result, std::string> get(const std::string& url);

    // Same as get(url) with extra request headers (e.g. Authorization
    // for the private SearXNG endpoint).
    static std::expected<Result, std::string>
    get(const std::string& url,
        const std::vector<std::pair<std::string, std::string>>& headers);
};
