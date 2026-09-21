#include "tools/SearxngSearchTool.h"

#include "tools/SearxngSearch.h"

#include <algorithm>

json SearxngSearchTool::get_definition() const {
    // Named json variables, NOT one deep nested literal — a brace
    // mis-count silently flips an object into an ARRAY and strict
    // providers 400 every turn (Phase 60c incident).
    const json properties = {
        {"query",
         {{"type", "string"}, {"description", "search query"}}},
        {"max_results",
         {{"type", "integer"},
          {"description", "number of results (default 10, max 25)"}}},
    };
    const json params = {{"type", "object"},
                         {"properties", properties},
                         {"required", json::array({"query"})}};
    const json fn = {{"name", get_name()},
                     {"description", get_description()},
                     {"parameters", params}};
    return {{"type", "function"}, {"function", fn}};
}

json SearxngSearchTool::execute(const json& args, FileIO& /*file_io*/) {
    // contains()+is_*() guards — args are model-controlled JSON and
    // value() throws on a type mismatch (type_error.306 class).
    std::string query;
    if (args.contains("query") && args["query"].is_string())
        query = args["query"].get<std::string>();
    if (query.empty()) return {{"status", "error"}, {"error", "missing query"}};

    int max_results = 10;
    if (args.contains("max_results") && args["max_results"].is_number_integer())
        max_results = args["max_results"].get<int>();
    max_results = std::clamp(max_results, 1, 25);

    auto results =
        SearxngSearch::search(query, static_cast<std::size_t>(max_results));
    if (!results) return {{"status", "error"}, {"error", results.error()}};

    json items = json::array();
    for (const auto& r : *results)
        items.push_back(
            {{"title", r.title}, {"url", r.url}, {"snippet", r.snippet}});
    return {{"status", "ok"}, {"query", query}, {"results", items}};
}
