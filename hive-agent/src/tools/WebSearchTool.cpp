#include "tools/WebSearchTool.h"

#include "tools/WebSearch.h"

#include <algorithm>

json WebSearchTool::execute(const json& args, FileIO& /*file_io*/) {
    const std::string query = args.value("query", "");
    if (query.empty()) return {{"status", "error"}, {"error", "missing query"}};

    int max_results = args.value("max_results", 8);
    max_results = std::clamp(max_results, 1, 20);

    auto results = WebSearch::search(query, static_cast<std::size_t>(max_results));
    if (!results) return {{"status", "error"}, {"error", results.error()}};

    json items = json::array();
    for (const auto& r : *results)
        items.push_back(
            {{"title", r.title}, {"url", r.url}, {"snippet", r.snippet}});
    return {{"status", "ok"}, {"query", query}, {"results", items}};
}
