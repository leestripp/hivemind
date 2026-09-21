#include "tools/SearxngSearch.h"

#include "core/utils.h" // base64_encode
#include "tools/WebFetch.h"

#include <cstdlib>

namespace {

// Percent-encode a query component (space → %20, like curl_easy_escape).
// Same helper as WebSearch.cpp — file-local by design.
std::string url_encode(const std::string& s) {
    static constexpr char kHex[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(s.size() * 3);
    for (const unsigned char c : s) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' ||
            c == '~') {
            out += static_cast<char>(c);
        } else {
            out += '%';
            out += kHex[c >> 4];
            out += kHex[c & 0xF];
        }
    }
    return out;
}

const char* env_or_null(const char* name) {
    const char* v = std::getenv(name);
    return (v != nullptr && *v != '\0') ? v : nullptr;
}

} // namespace

std::expected<SearxngSearch::Config, std::string>
SearxngSearch::config_from_env() {
    Config c;

    c.base_url = env_or_null("HIVE_SECRET_SEARXNG_URL")
                     ? env_or_null("HIVE_SECRET_SEARXNG_URL")
                     : kDefaultBaseUrl;
    while (c.base_url.size() > 1 && c.base_url.back() == '/')
        c.base_url.pop_back();
    if (!c.base_url.starts_with("https://"))
        return std::unexpected(
            "SearXNG base URL must be https:// (HIVE_SECRET_SEARXNG_URL is "
            "not TLS); plain http is rejected by the instance");

    c.username = env_or_null("HIVE_SECRET_SEARXNG_USER")
                     ? env_or_null("HIVE_SECRET_SEARXNG_USER")
                     : kDefaultUsername;

    if (const char* pass = env_or_null("HIVE_SECRET_SEARXNG_PASS")) {
        c.password = pass;
    } else {
        return std::unexpected(
            "SearXNG credentials not configured — grant a login secret "
            "named 'searxng' to this agent on the hub "
            "(HIVE_SECRET_SEARXNG_PASS is not set)");
    }
    return c;
}

std::expected<std::vector<SearxngSearch::Result>, std::string>
SearxngSearch::parse(const std::string& body, std::size_t max_results) {
    const json doc = json::parse(body, nullptr, false);
    if (doc.is_discarded())
        return std::unexpected("invalid JSON in SearXNG response");
    // contains()+is_array() guard — value() on a mismatched type throws
    // (type_error.306 incident, 2026-09-02).
    if (!doc.contains("results") || !doc["results"].is_array())
        return std::unexpected("SearXNG response has no results array");

    std::vector<Result> out;
    for (const auto& item : doc["results"]) {
        if (out.size() >= max_results) break;
        if (!item.is_object()) continue;

        Result r;
        if (item.contains("title") && item["title"].is_string())
            r.title = item["title"].get<std::string>();
        if (item.contains("url") && item["url"].is_string())
            r.url = item["url"].get<std::string>();
        if (item.contains("content") && item["content"].is_string())
            r.snippet = item["content"].get<std::string>();

        if (!r.url.empty() && !r.title.empty()) out.push_back(std::move(r));
    }
    return out;
}

std::expected<std::vector<SearxngSearch::Result>, std::string>
SearxngSearch::search(const std::string& query, std::size_t max_results) {
    if (query.empty()) return std::unexpected("empty query");

    const auto cfg = config_from_env();
    if (!cfg) return std::unexpected(cfg.error());

    const std::string auth =
        "Basic " + base64_encode(cfg->username + ":" + cfg->password);
    auto page = WebFetch::get(cfg->base_url +
                                  "/search?q=" + url_encode(query) +
                                  "&format=json",
                              {{"Authorization", auth}});
    if (!page) {
        std::string err = page.error();
        // The most likely operator error is a missing/stale grant —
        // name it instead of handing back a bare status code.
        if (err == "HTTP 401" || err == "HTTP 403")
            err += " — SearXNG rejected the credentials; check the "
                   "'searxng' login secret granted to this agent";
        return std::unexpected(err);
    }
    return parse(page->body, max_results);
}
