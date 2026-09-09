#pragma once

#include "tools/BaseTool.h"

// Tab-less HTTP fetch for online research. HTML pages are converted to
// plain text (HtmlToText); everything else (markdown, JSON, plain text)
// passes through. Ported from MonkeyByte (ui/chat/WebFetchTool)
// 2026-09-09 — restored after the daedalus in-tree merge left hive-agent
// without web tools.
class WebFetchTool : public BaseTool {
public:
    std::string get_name() const override { return "web_fetch"; }

    std::string get_description() const override {
        return "Fetch a URL over HTTP(S) and return its content as text "
               "(HTML pages are converted to plain text). Use for static "
               "documentation and reference pages; JavaScript-rendered "
               "pages will come back empty or incomplete — delegate those "
               "to a browser-capable agent instead.";
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
                 {{"url",
                   {{"type", "string"},
                    {"description", "http:// or https:// URL to GET"}}},
                  {"max_chars",
                   {{"type", "integer"},
                    {"description",
                     "truncate the returned text (default 8000, max "
                     "20000)"}}}}},
                {"required", json::array({"url"})}}}}},
        };
    }

    json execute(const json& args, FileIO& file_io) override;
};
