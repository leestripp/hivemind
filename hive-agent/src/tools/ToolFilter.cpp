#include "tools/ToolFilter.h"

#include <string>

bool ToolFilter::is_tool_allowed(const std::string& tool_name) const {
    if (disabled_tools.count(tool_name)) {
        return false;
    }

    const std::string server = extract_mcp_server(tool_name);
    if (!server.empty() && disabled_mcp_servers.count(server)) {
        return false;
    }

    return true;
}

nlohmann::json ToolFilter::filter_definitions(
    const nlohmann::json& all_defs) const {
    nlohmann::json out = nlohmann::json::array();
    if (!all_defs.is_array()) {
        return out;
    }

    for (const auto& def : all_defs) {
        std::string tool_name;
        if (def.contains("function") && def["function"].contains("name")) {
            tool_name = def["function"]["name"].get<std::string>();
        } else if (def.contains("name")) {
            tool_name = def["name"].get<std::string>();
        }

        if (!tool_name.empty() && !is_tool_allowed(tool_name)) {
            continue;
        }
        out.push_back(def);
    }

    return out;
}

std::string ToolFilter::extract_mcp_server(const std::string& tool_name) {
    constexpr std::string_view prefix = "mcp__";
    if (!tool_name.starts_with(prefix)) {
        return {};
    }

    const std::string_view body = std::string_view(tool_name).substr(prefix.size());
    const auto pos = body.find("__");
    if (pos == std::string_view::npos) {
        return {};
    }

    return std::string(body.substr(0, pos));
}
