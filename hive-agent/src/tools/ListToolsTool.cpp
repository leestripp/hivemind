/**
 * @file src/tools/ListToolsTool.cpp
 * @brief §LIST-TOOLS (2026-09-20): see ListToolsTool.h.
 */
#include "ListToolsTool.h"

#include "ToolHandler.h"
#include "ToolFactory.h"

#include <algorithm>
#include <string>

std::string ListToolsTool::get_description() const {
    return "List the tools you can actually call right now — the ground "
           "truth from the live tool registry (name + one-line description "
           "each). ALWAYS call this when asked what tools you have, instead "
           "of answering from memory: your conversation history may predate "
           "tools that were added later, so recollection can be wrong. Also "
           "reports tools disabled by host policy, if any.";
}

json ListToolsTool::get_definition() const {
    return json::object({
        {"type", "function"},
        {"function", {
            {"name", get_name()},
            {"description", get_description()},
            {"parameters", json::object({
                {"type", "object"},
                {"properties", json::object()}
            })}
        }}
    });
}

json ListToolsTool::execute(const json& /*args*/, FileIO& /*file_io*/) {
    auto factory = ToolHandler::get_tool_factory();
    if (!factory) {
        return json::object({{"status", "error"},
                             {"message", "Tool factory not initialised"}});
    }

    const json defs = factory->get_all_definitions();

    json available = json::array();
    json disabled  = json::array();
    for (const auto& def : defs) {
        std::string name;
        std::string description;
        if (def.contains("function") && def["function"].is_object()) {
            const auto& fn = def["function"];
            if (fn.contains("name") && fn["name"].is_string())
                name = fn["name"].get<std::string>();
            if (fn.contains("description") && fn["description"].is_string())
                description = fn["description"].get<std::string>();
        } else if (def.contains("name") && def["name"].is_string()) {
            name = def["name"].get<std::string>();
            if (def.contains("description") && def["description"].is_string())
                description = def["description"].get<std::string>();
        }
        if (name.empty()) continue;

        // One line, capped — the model needs the name and the gist, not
        // the full marketing copy.
        const auto nl = description.find('\n');
        if (nl != std::string::npos) description.erase(nl);
        if (description.size() > 160) {
            description.erase(160);
            description += "...";
        }

        if (active_filter_ && !active_filter_->is_tool_allowed(name)) {
            disabled.push_back(name);
            continue;
        }
        available.push_back({{"name", name}, {"description", description}});
    }

    std::sort(disabled.begin(), disabled.end());

    json result = {
        {"status", "success"},
        {"count", available.size()},
        {"tools", available},
        {"note", "Authoritative live registry — a tool NOT in this list "
                 "cannot be called, and every tool in this list CAN be "
                 "called. Do not second-guess it from conversation history."}
    };
    if (!disabled.empty()) {
        result["disabled_by_host"] = disabled;
    }
    return result;
}
