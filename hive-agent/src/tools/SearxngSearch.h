#pragma once

#include <expected>
#include <string>
#include <vector>

// Web search via our private SearXNG metasearch instance on the
// leestrippai.com droplet (nginx :8443 -> SearXNG 127.0.0.1:8888,
// HTTP Basic auth, JSON API: GET /search?q=...&format=json).
//
// Config comes from the hub-granted secret environment (Phase 60):
// grant a login-type secret named "searxng" to the agent and the hub
// renders HIVE_SECRET_SEARXNG_{USER,PASS,URL} into its container:
//   HIVE_SECRET_SEARXNG_URL  (default https://leestrippai.com:8443)
//   HIVE_SECRET_SEARXNG_USER (default agent)
//   HIVE_SECRET_SEARXNG_PASS (required — descriptive error when absent)
//
// The parser is separate from the network call so it can be unit-tested
// against canned JSON. Mirrors the WebSearch (DuckDuckGo) split.
class SearxngSearch {
public:
    struct Result {
        std::string title;
        std::string url;
        std::string snippet; // SearXNG "content" field; may be empty
    };

    struct Config {
        std::string base_url;
        std::string username;
        std::string password;
    };

    static constexpr const char* kDefaultBaseUrl =
        "https://leestrippai.com:8443";
    static constexpr const char* kDefaultUsername = "agent";

    // Resolve config from the environment. Errors when the password is
    // missing or the URL is not https:// (SearXNG rejects plain-http
    // result URLs with InvalidUrl; the nginx vhost only serves TLS).
    // The password is never included in error strings.
    static std::expected<Config, std::string> config_from_env();

    // Pure parser for a SearXNG format=json response body — no network.
    // Extracts title/url/content per result; tolerates missing content
    // and non-string fields. Errors on invalid JSON or a missing
    // "results" array.
    static std::expected<std::vector<Result>, std::string>
    parse(const std::string& body, std::size_t max_results);

    // GET <base_url>/search?q=<urlencoded>&format=json with HTTP Basic
    // auth via WebFetch, then parse. Returns up to max_results entries.
    static std::expected<std::vector<Result>, std::string>
    search(const std::string& query, std::size_t max_results = 10);
};
