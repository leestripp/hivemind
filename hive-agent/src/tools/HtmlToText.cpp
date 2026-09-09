#include "tools/HtmlToText.h"

#include <algorithm>
#include <cctype>
#include <string_view>

namespace {

bool iequals(std::string_view a, std::string_view b) {
    return a.size() == b.size() &&
           std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
               return std::tolower(static_cast<unsigned char>(x)) ==
                      std::tolower(static_cast<unsigned char>(y));
           });
}

// Case-insensitive search; returns npos when not found.
std::size_t ifind(const std::string& haystack, std::string_view needle,
                  std::size_t from = 0) {
    const auto it = std::search(haystack.begin() + static_cast<long>(from),
                                haystack.end(), needle.begin(), needle.end(),
                                [](char x, char y) {
                                    return std::tolower(static_cast<unsigned char>(x)) ==
                                           std::tolower(static_cast<unsigned char>(y));
                                });
    return it == haystack.end()
               ? std::string::npos
               : static_cast<std::size_t>(std::distance(haystack.begin(), it));
}

// Block-level closing/standalone tags become line breaks so paragraphs and
// list items don't fuse into one line.
bool is_block_tag(std::string_view tag) {
    static constexpr std::string_view kBlocks[] = {
        "p", "div", "br", "hr", "li", "ul", "ol", "table", "tr", "td", "th",
        "h1", "h2", "h3", "h4", "h5", "h6", "header", "footer", "section",
        "article", "aside", "nav", "main", "pre", "blockquote", "form",
        "fieldset", "figure", "figcaption", "dt", "dd",
    };
    for (const auto b : kBlocks)
        if (iequals(tag, b)) return true;
    return false;
}

// Decode one entity starting at html[pos] (which is '&'). Returns the
// decoded text and advances pos past the ';' (or leaves it on '&' when the
// entity is not recognized / unterminated).
std::string decode_entity(const std::string& html, std::size_t& pos) {
    const std::size_t semi = html.find(';', pos + 1);
    if (semi == std::string::npos || semi - pos > 10) return "&";
    const std::string_view ent(html.data() + pos + 1, semi - pos - 1);
    std::string out;
    if (ent == "amp") out = "&";
    else if (ent == "lt") out = "<";
    else if (ent == "gt") out = ">";
    else if (ent == "quot") out = "\"";
    else if (ent == "apos" || ent == "#39") out = "'";
    else if (ent == "nbsp") out = " ";
    else if (ent.starts_with("#")) {
        try {
            const long code = ent.starts_with("#x") || ent.starts_with("#X")
                                  ? std::stol(std::string(ent.substr(2)), nullptr, 16)
                                  : std::stol(std::string(ent.substr(1)));
            if (code > 0 && code < 128)
                out = static_cast<char>(code); // ASCII only; above → drop
        } catch (...) {
            return "&";
        }
    } else {
        return "&"; // unknown named entity: keep the '&'
    }
    pos = semi;
    return out;
}

} // namespace

std::string HtmlToText::to_text(const std::string& html) {
    std::string out;
    out.reserve(html.size());

    std::size_t pos = 0;
    while (pos < html.size()) {
        if (html[pos] == '<') {
            // Drop script/style/noscript elements including their content.
            bool dropped = false;
            for (const char* drop : {"script", "style", "noscript"}) {
                const std::size_t len = std::string_view(drop).size();
                if (pos + 1 + len <= html.size() &&
                    iequals(std::string_view(html).substr(pos + 1, len), drop) &&
                    pos + 1 + len < html.size() &&
                    (html[pos + 1 + len] == '>' || html[pos + 1 + len] == ' ' ||
                     html[pos + 1 + len] == '\t' || html[pos + 1 + len] == '\n' ||
                     html[pos + 1 + len] == '/')) {
                    const std::string closer = "</" + std::string(drop);
                    const std::size_t end = ifind(html, closer, pos);
                    if (end == std::string::npos) {
                        pos = html.size(); // unterminated: drop the rest
                    } else {
                        const std::size_t gt = html.find('>', end);
                        pos = gt == std::string::npos ? html.size() : gt + 1;
                    }
                    dropped = true;
                    break;
                }
            }
            if (dropped) continue;
            // Skip comments.
            if (html.compare(pos, 4, "<!--") == 0) {
                const std::size_t end = html.find("-->", pos + 4);
                pos = end == std::string::npos ? html.size() : end + 3;
                continue;
            }
            const std::size_t gt = html.find('>', pos + 1);
            if (gt == std::string::npos) break; // unterminated tag: drop
            // Tag name (after optional '/') decides block vs inline.
            std::size_t name_start = pos + 1;
            if (name_start < gt && html[name_start] == '/') ++name_start;
            std::size_t name_end = name_start;
            while (name_end < gt && !std::isspace(
                       static_cast<unsigned char>(html[name_end])) &&
                   html[name_end] != '/' && html[name_end] != '>')
                ++name_end;
            const std::string_view tag(html.data() + name_start,
                                       name_end - name_start);
            if (is_block_tag(tag) && !out.empty() && out.back() != '\n')
                out += '\n';
            pos = gt + 1;
            continue;
        }
        if (html[pos] == '&') {
            out += decode_entity(html, pos);
            ++pos;
            continue;
        }
        out += html[pos++];
    }

    // Collapse horizontal whitespace per line, then blank-line runs.
    std::string cleaned;
    cleaned.reserve(out.size());
    bool pending_space = false;
    int newlines = 0;
    for (const char c : out) {
        if (c == '\n') {
            ++newlines;
            pending_space = false;
            continue;
        }
        if (newlines > 0) {
            if (!cleaned.empty())
                cleaned += newlines > 1 ? "\n\n" : "\n";
            newlines = 0;
        }
        if (c == ' ' || c == '\t' || c == '\r') {
            pending_space = !cleaned.empty() && cleaned.back() != '\n';
            continue;
        }
        if (pending_space) {
            cleaned += ' ';
            pending_space = false;
        }
        cleaned += c;
    }
    // Trim trailing whitespace/newlines.
    while (!cleaned.empty() &&
           std::isspace(static_cast<unsigned char>(cleaned.back())))
        cleaned.pop_back();
    return cleaned;
}
