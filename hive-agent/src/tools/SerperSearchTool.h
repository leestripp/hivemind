#pragma once

#include "BaseTool.h"
#include <string>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

/**
 * SerperSearchTool - Web search via the Serper.dev Google API.
 * Usage: serper_search(query)
 *
 * §SERPER-CORE (2026-08-02): moved back into the core library from
 * demos/example-tools. Its configuration (serper_config table) and key
 * lookup (core/SerperConfig) have always lived in core, so the tool is
 * registered by default; without a configured key it returns a
 * descriptive error instead of executing.
 */
class SerperSearchTool : public BaseTool {
public:
    std::string get_name() const override { return "serper_search"; }
    std::string get_description() const override { return "Perform web search using Google via Serper.dev API"; }
    json get_definition() const override;
    json execute(const json& args, FileIO& file_io) override;
};
