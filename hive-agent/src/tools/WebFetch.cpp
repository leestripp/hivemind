#include "tools/WebFetch.h"

#include <http/HttpClient.h>

#include <algorithm>
#include <cctype>

std::expected<WebFetch::Result, std::string> WebFetch::get(const std::string& url) {
    return get(url, {});
}

std::expected<WebFetch::Result, std::string>
WebFetch::get(const std::string& url,
              const std::vector<std::pair<std::string, std::string>>& headers) {
    if (!url.starts_with("http://") && !url.starts_with("https://"))
        return std::unexpected("only http:// and https:// URLs are supported");

    OmniNet::HTTP::ClientConfig cfg;
    cfg.user_agent = "hive-agent/1.0";
    cfg.connect_timeout = std::chrono::seconds(10);
    cfg.request_timeout = std::chrono::seconds(10);
    cfg.follow_redirects = true;
    cfg.max_redirect_hops = 5;

    OmniNet::HTTP::ClientRequest req;
    req.method("GET").url(url);
    for (const auto& [name, value] : headers) req.header(name, value);

    auto resp = OmniNet::HTTP::Client::get_instance().send_blocking(req, cfg);
    if (!resp)
        return std::unexpected(std::string("HTTP error: ") +
                               OmniNet::HTTP::clientErrorName(resp.error()));

    Result result;
    result.status = resp->statusCode();
    if (result.status < 200 || result.status >= 300)
        return std::unexpected("HTTP " + std::to_string(result.status));

    result.content_type = resp->header("Content-Type"); // case-insensitive
    const auto semi = result.content_type.find(';');
    if (semi != std::string::npos) result.content_type.resize(semi);
    std::ranges::transform(result.content_type, result.content_type.begin(),
                           [](unsigned char c) { return std::tolower(c); });

    result.body = resp->bodyAsString();
    if (result.body.size() > kMaxBody)
        return std::unexpected("response too large (over 2 MB)");
    return result;
}
