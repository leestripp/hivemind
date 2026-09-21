#pragma once

#include "tools/BaseTool.h"

// Key-less web search for online research, via the DuckDuckGo HTML
// endpoint (WebSearch). Follow up with web_fetch on a result URL.
// Ported from MonkeyByte (ui/chat/WebSearchTool) 2026-09-09 — restored
// after the daedalus in-tree merge left hive-agent without web tools.
class WebSearchTool : public BaseTool {
public:
    std::string get_name() const override { return "web_search"; }

    std::string get_description() const override {
        return "Search the web (DuckDuckGo, no API key) and return result "
               "titles, URLs, and snippets. Fallback for searxng_search "
               "(our private instance) when it errors or returns thin "
               "results; then call web_fetch on a result URL to read the "
               "page.";
    }

    json get_definition() const override {
        return {
            {"type", "function"},
            {"function",
             {{"name", get_name()},
              {"description", get_description()},
              {"parameters",
               {{"type", "object"},
                {"properties",
                 {{"query",
                   {{"type", "string"},
                    {"description", "search query"}}},
                  {"max_results",
                   {{"type", "integer"},
                    {"description",
                     "number of results (default 8, max 20)"}}}}},
                {"required", json::array({"query"})}}}}},
        };
    }

    json execute(const json& args, FileIO& file_io) override;
};
