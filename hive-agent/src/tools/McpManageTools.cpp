#include "tools/McpManageTools.h"

#include "core/settings.h"
#include "mcp/McpClient.h"   // ToolDefinition
#include "mcp/McpServerConfig.h"
#include "mcp/McpServerManager.h"
#include "tools/ToolHandler.h"

#include <regex>

namespace {

constexpr const char* kHubReadonlyMsg =
    "managed by the hub roster (origin=hub) — ask the user to change it; "
    "agents may only mcp_add/mcp_enable/mcp_disable/mcp_remove their own "
    "origin=local servers";

mcp::McpServerConfig to_config(const hive::SettingsStore::McpServer& s)
{
    mcp::McpServerConfig c;
    c.name         = s.name;
    c.transport    = s.transport;
    c.command      = s.command;
    c.args         = s.args;
    c.env          = s.env;
    c.url          = s.url;
    c.bearer_token = s.bearer_token;
    c.description  = s.description;
    c.enabled      = s.enabled;
    return c;
}

bool valid_name(const std::string& n)
{
    static const std::regex re(R"(^[a-z0-9][a-z0-9_-]{0,63}$)");
    return std::regex_match(n, re);
}

json err(const std::string& msg)
{
    json j; j["status"] = "error"; j["error"] = msg; return j;
}

json tool_summary(mcp::McpServerManager* mgr, const std::string& name)
{
    json tools = json::array();
    if (mgr && mgr->is_connected(name)) {
        for (const auto& t : mgr->get_tools(name)) {
            json tj; tj["name"] = t.name; tj["description"] = t.description;
            tools.push_back(std::move(tj));
            if (tools.size() >= 50) break;  // cap: some servers expose hundreds
        }
    }
    return tools;
}

json server_json(mcp::McpServerManager* mgr,
                 const hive::SettingsStore::McpServer& s)
{
    json j;
    j["name"]        = s.name;
    j["transport"]   = s.transport;
    j["origin"]      = s.origin;
    j["enabled"]     = s.enabled;
    if (!s.command.empty()) j["command"] = s.command;
    if (!s.args.empty())    j["args"]    = s.args;
    if (!s.url.empty())     j["url"]     = s.url;
    if (!s.description.empty()) j["description"] = s.description;
    // env: keys only — values may be secrets and must never reach the LLM.
    json env_keys = json::array();
    for (const auto& [k, v] : s.env) env_keys.push_back(k);
    if (!env_keys.empty()) j["env_keys"] = env_keys;
    const bool connected = mgr && mgr->is_connected(s.name);
    j["connected"] = connected;
    if (connected) {
        j["tools"] = tool_summary(mgr, s.name);
    } else if (s.enabled && mgr) {
        j["last_error"] = mgr->get_last_error();
    }
    return j;
}

}  // namespace

// ------------------------------------------------------------------ mcp_list

std::string McpListTool::get_description() const
{
    return "List configured MCP servers with live connection state and "
           "exposed tools. Shows origin (hub = roster-managed, read-only; "
           "local = yours to manage), enabled flag, and env var KEYS only "
           "(values are redacted).";
}

json McpListTool::get_definition() const
{
    json schema;
    schema["type"] = "object";
    schema["properties"] = json::object();
    json def;
    def["type"] = "function";
    def["function"]["name"] = get_name();
    def["function"]["description"] = get_description();
    def["function"]["parameters"] = std::move(schema);
    return def;
}

json McpListTool::execute(const json&, FileIO&)
{
    json out;
    json servers = json::array();
    for (const auto& s : svc_.store->list_mcp_servers())
        servers.push_back(server_json(svc_.mgr, s));
    out["status"] = "success";
    out["servers"] = std::move(servers);
    return out;
}

// ------------------------------------------------------------------ mcp_add

std::string McpAddTool::get_description() const
{
    return "Register a NEW MCP server and connect to it immediately. "
           "Install the server first with the bash tool (pip/uv/npm/git "
           "inside this container), then call this. For stdio servers "
           "provide command + args; for remote servers provide url. The "
           "server persists across container recreations (origin=local). "
           "On success the discovered tool list is returned — verify with "
           "one real tool call before relying on it.";
}

json McpAddTool::get_definition() const
{
    json props;
    props["name"]["type"] = "string";
    props["name"]["description"] =
        "Server id: lowercase [a-z0-9_-], e.g. 'comfy'. Tools surface as "
        "mcp__<name>__<tool>.";
    props["command"]["type"] = "string";
    props["command"]["description"] =
        "stdio: executable to spawn (e.g. 'comfy-mcp', 'uvx', 'npx').";
    props["args"]["type"] = "array";
    props["args"]["items"]["type"] = "string";
    props["args"]["description"] = "stdio: argv for the command.";
    props["env"]["type"] = "object";
    props["env"]["additionalProperties"]["type"] = "string";
    props["env"]["description"] =
        "stdio: environment variables (API keys etc). Stored container-locally.";
    props["url"]["type"] = "string";
    props["url"]["description"] =
        "http transport: endpoint URL (e.g. http://127.0.0.1:8765/mcp). "
        "When set, command is ignored.";
    props["description"]["type"] = "string";
    props["description"]["description"] = "Human-readable purpose.";

    json schema;
    schema["type"] = "object";
    schema["properties"] = std::move(props);
    schema["required"] = json::array({"name"});

    json def;
    def["type"] = "function";
    def["function"]["name"] = get_name();
    def["function"]["description"] = get_description();
    def["function"]["parameters"] = std::move(schema);
    return def;
}

json McpAddTool::execute(const json& args, FileIO&)
{
    const std::string name = args.value("name", "");
    if (!valid_name(name))
        return err("invalid name '" + name +
                   "': use lowercase [a-z0-9_-], 1-64 chars");

    if (auto existing = svc_.store->get_mcp_server(name)) {
        if (existing->origin == "hub")
            return err(std::string("server '") + name + "' is " + kHubReadonlyMsg);
        return err("server '" + name +
                   "' already exists (origin=local) — use mcp_enable, or "
                   "mcp_remove it first to redefine");
    }

    hive::SettingsStore::McpServer s;
    s.name = name;
    s.command = args.value("command", "");
    s.url = args.value("url", "");
    if (s.command.empty() && s.url.empty())
        return err("provide 'command' (stdio) or 'url' (http)");
    s.transport = s.url.empty() ? "stdio" : "streamable-http";
    s.description = args.value("description", "");
    s.enabled = true;
    s.origin = "local";
    if (args.contains("args") && args["args"].is_array())
        for (const auto& a : args["args"])
            s.args.push_back(a.get<std::string>());
    if (args.contains("env") && args["env"].is_object())
        for (auto it = args["env"].begin(); it != args["env"].end(); ++it)
            s.env[it.key()] = it.value().get<std::string>();

    svc_.store->upsert_mcp_server(s);

    json out;
    out["status"] = "success";
    out["persisted"] = true;
    if (svc_.mgr->enable_server(to_config(s))) {
        out["connected"] = true;
        out["tools"] = tool_summary(svc_.mgr, name);
        out["message"] = "server connected — its tools are available as "
                         "mcp__" + name + "__<tool> from the next LLM call";
    } else {
        out["connected"] = false;
        out["error"] = svc_.mgr->get_last_error();
        out["message"] = "persisted but connection failed — fix the "
                         "command/url and mcp_enable, or mcp_remove it";
    }
    return out;
}

// ----------------------------------------------------------------- mcp_enable

std::string McpEnableTool::get_description() const
{
    return "Enable and connect to a configured MCP server (also works on "
           "hub-roster servers to reconnect after a transient failure). "
           "Use mcp_list to see current state.";
}

json McpEnableTool::get_definition() const
{
    json props;
    props["name"]["type"] = "string";
    props["name"]["description"] = "Server name from mcp_list.";
    json schema;
    schema["type"] = "object";
    schema["properties"] = std::move(props);
    schema["required"] = json::array({"name"});
    json def;
    def["type"] = "function";
    def["function"]["name"] = get_name();
    def["function"]["description"] = get_description();
    def["function"]["parameters"] = std::move(schema);
    return def;
}

json McpEnableTool::execute(const json& args, FileIO&)
{
    const std::string name = args.value("name", "");
    auto existing = svc_.store->get_mcp_server(name);
    if (!existing)
        return err("unknown server '" + name + "' — see mcp_list");

    existing->enabled = true;
    svc_.store->upsert_mcp_server(*existing);

    json out;
    out["status"] = "success";
    if (svc_.mgr->enable_server(to_config(*existing))) {
        out["connected"] = true;
        out["tools"] = tool_summary(svc_.mgr, name);
    } else {
        out["connected"] = false;
        out["error"] = svc_.mgr->get_last_error();
    }
    return out;
}

// ---------------------------------------------------------------- mcp_disable

std::string McpDisableTool::get_description() const
{
    return "Disconnect an MCP server and keep it disabled across "
           "restarts (local servers only — hub-roster servers are "
           "managed by the user).";
}

json McpDisableTool::get_definition() const
{
    json props;
    props["name"]["type"] = "string";
    props["name"]["description"] = "Server name from mcp_list.";
    json schema;
    schema["type"] = "object";
    schema["properties"] = std::move(props);
    schema["required"] = json::array({"name"});
    json def;
    def["type"] = "function";
    def["function"]["name"] = get_name();
    def["function"]["description"] = get_description();
    def["function"]["parameters"] = std::move(schema);
    return def;
}

json McpDisableTool::execute(const json& args, FileIO&)
{
    const std::string name = args.value("name", "");
    auto existing = svc_.store->get_mcp_server(name);
    if (!existing)
        return err("unknown server '" + name + "' — see mcp_list");
    if (existing->origin == "hub")
        return err("server '" + name + "' is " + kHubReadonlyMsg);

    svc_.mgr->disable_server(name);
    existing->enabled = false;
    svc_.store->upsert_mcp_server(*existing);

    json out;
    out["status"] = "success";
    out["connected"] = false;
    out["persisted"] = true;
    return out;
}

// ----------------------------------------------------------------- mcp_remove

std::string McpRemoveTool::get_description() const
{
    return "Disconnect AND permanently delete an MCP server registration "
           "(local servers only — hub-roster servers are managed by the "
           "user). The server software itself, if installed via bash, is "
           "not uninstalled.";
}

json McpRemoveTool::get_definition() const
{
    json props;
    props["name"]["type"] = "string";
    props["name"]["description"] = "Server name from mcp_list.";
    json schema;
    schema["type"] = "object";
    schema["properties"] = std::move(props);
    schema["required"] = json::array({"name"});
    json def;
    def["type"] = "function";
    def["function"]["name"] = get_name();
    def["function"]["description"] = get_description();
    def["function"]["parameters"] = std::move(schema);
    return def;
}

json McpRemoveTool::execute(const json& args, FileIO&)
{
    const std::string name = args.value("name", "");
    auto existing = svc_.store->get_mcp_server(name);
    if (!existing)
        return err("unknown server '" + name + "' — see mcp_list");
    if (existing->origin == "hub")
        return err("server '" + name + "' is " + kHubReadonlyMsg);

    svc_.mgr->disable_server(name);
    svc_.store->delete_mcp_server(name);

    json out;
    out["status"] = "success";
    out["removed"] = name;
    return out;
}
