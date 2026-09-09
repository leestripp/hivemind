#include "tools/WebSearch.h"

#include "tools/HtmlToText.h"
#include "tools/WebFetch.h"

namespace {

// Percent-encode a query component (space → %20, like curl_easy_escape).
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

// Percent-decode a URL component (uddg targets are URL-encoded).
std::string url_decode(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size()) {
            try {
                out += static_cast<char>(
                    std::stoi(s.substr(i + 1, 2), nullptr, 16));
                i += 2;
                continue;
            } catch (...) {
            }
        }
        if (s[i] == '+') {
            out += ' ';
            continue;
        }
        out += s[i];
    }
    return out;
}

// DDG wraps outbound links as //duckduckgo.com/l/?uddg=<encoded>&rut=… —
// decode the uddg parameter; anything else passes through (scheme-relative
// //… gets an https: prefix so the agent can fetch it directly).
std::string resolve_href(const std::string& href) {
    if (const auto pos = href.find("uddg="); pos != std::string::npos) {
        std::string uddg = href.substr(pos + 5);
        if (const auto amp = uddg.find('&'); amp != std::string::npos)
            uddg.resize(amp);
        return url_decode(uddg);
    }
    if (href.starts_with("//")) return "https:" + href;
    return href;
}

// Extract the href="…" attribute of the anchor tag starting at lt.
std::string href_of(const std::string& html, std::size_t lt, std::size_t gt) {
    const auto href = html.find("href=", lt);
    if (href == std::string::npos || href > gt) return "";
    std::size_t val = href + 5;
    if (val < html.size() && (html[val] == '"' || html[val] == '\'')) {
        const char quote = html[val++];
        const auto end = html.find(quote, val);
        if (end == std::string::npos || end > gt) return "";
        return html.substr(val, end - val);
    }
    const auto end = html.find_first_of(" \t>", val);
    return html.substr(val, end == std::string::npos ? gt : end - val);
}

// Inner HTML between gt+1 and the matching </a> (or element end).
std::string inner_html(const std::string& html, std::size_t gt,
                       std::string_view closer) {
    const auto end = html.find(closer, gt);
    if (end == std::string::npos) return "";
    return html.substr(gt + 1, end - gt - 1);
}

} // namespace

std::vector<WebSearch::Result> WebSearch::parse(const std::string& html,
                                                std::size_t max_results) {
    std::vector<Result> out;
    std::size_t pos = 0;
    while (out.size() < max_results) {
        // Result anchors carry class="result__a".
        const auto cls = html.find("result__a", pos);
        if (cls == std::string::npos) break;
        const auto lt = html.rfind('<', cls);
        const auto gt = html.find('>', cls);
        if (lt == std::string::npos || gt == std::string::npos) break;

        Result r;
        r.url = resolve_href(href_of(html, lt, gt));
        r.title = HtmlToText::to_text(inner_html(html, gt, "</a>"));

        // The snippet follows the anchor within the same result block.
        const auto snip = html.find("result__snippet", gt);
        if (snip != std::string::npos) {
            const auto sgt = html.find('>', snip);
            if (sgt != std::string::npos) {
                // Snippet element is an <a> or <td>; both close plainly.
                auto text = inner_html(html, sgt, "</a>");
                if (text.empty()) text = inner_html(html, sgt, "</td>");
                r.snippet = HtmlToText::to_text(text);
            }
        }

        if (!r.url.empty() && !r.title.empty()) out.push_back(std::move(r));
        pos = gt + 1;
    }
    return out;
}

std::expected<std::vector<WebSearch::Result>, std::string>
WebSearch::search(const std::string& query, std::size_t max_results) {
    if (query.empty()) return std::unexpected("empty query");

    auto page = WebFetch::get("https://html.duckduckgo.com/html/?q=" +
                              url_encode(query));
    if (!page) return std::unexpected(page.error());
    return parse(page->body, max_results);
}
