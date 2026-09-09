#include "tools/WebFetchTool.h"

#include "tools/HtmlToText.h"
#include "tools/WebFetch.h"

#include <algorithm>

json WebFetchTool::execute(const json& args, FileIO& /*file_io*/) {
    const std::string url = args.value("url", "");
    if (url.empty()) return {{"status", "error"}, {"error", "missing url"}};

    auto page = WebFetch::get(url);
    if (!page) return {{"status", "error"}, {"error", page.error()}};

    std::string text = page->content_type.contains("html")
                           ? HtmlToText::to_text(page->body)
                           : page->body;
    int max_chars = args.value("max_chars", 8000);
    max_chars = std::clamp(max_chars, 256, 20000);

    json out{{"status", "ok"},
             {"url", url},
             {"content_type", page->content_type}};
    if (static_cast<int>(text.size()) > max_chars) {
        text.resize(static_cast<std::size_t>(max_chars));
        out["truncated"] = true;
    }
    out["text"] = text;
    return out;
}
