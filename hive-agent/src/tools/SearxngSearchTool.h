#pragma once

#include "tools/BaseTool.h"

// Preferred web search, via our self-hosted SearXNG metasearch on the
// leestrippai.com droplet (SearxngSearch) — keyless for the agent (the
// hub injects HTTP Basic credentials as a granted login secret), no
// per-query billing, not bot-blocked. web_search (DuckDuckGo) remains as
// the credential-free fallback. Added 2026-09-21 alongside the SearXNG
// deployment on the shared droplet.
class SearxngSearchTool : public BaseTool {
public:
    std::string get_name() const override { return "searxng_search"; }

    std::string get_description() const override {
        return "Search the web via our private SearXNG metasearch "
               "instance (self-hosted, no API key billing, not "
               "bot-blocked) and return result titles, URLs, and "
               "snippets. This is the preferred search tool; use "
               "web_search (DuckDuckGo) as a fallback if it errors. "
               "Then call web_fetch on a result URL to read the page.";
    }

    json get_definition() const override;

    json execute(const json& args, FileIO& file_io) override;
};
