#include "tools/McpProxiedTool.h"
#include "mcp/McpServerManager.h"

#include <algorithm>
#include <cctype>

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

McpProxiedTool::McpProxiedTool(std::string server_name, mcp::ToolDefinition def,
                                   mcp::McpServerManager* mcp_manager)
    : server_name_(std::move(server_name)),
      def_(std::move(def)),
      mcp_manager_(mcp_manager) {}

// ---------------------------------------------------------------------------
// Public interface
// ---------------------------------------------------------------------------

std::string McpProxiedTool::get_name() const {
    // Canonical MCP-host namespace: mcp__<server>__<tool>
    return "mcp__" + sanitize_name(server_name_) + "__" + sanitize_name(def_.name);
}

std::string McpProxiedTool::get_description() const {
    return def_.description;
}

json McpProxiedTool::get_definition() const {
    // OpenAI-compatible function schema. The model's tool dispatcher sees
    // the same shape it would for any other native tool, with the server's
    // own inputSchema piped straight through as `parameters`.
    json parameters = def_.input_schema.is_object()
                       ? def_.input_schema
                       : json::object();

    return json::object({
        {"type", "function"},
        {"function", json::object({
            {"name",        get_name()},
            {"description", def_.description},
            {"parameters",  parameters}
        })}
    });
}

json McpProxiedTool::execute(const json& args, FileIO& file_io) {
    return execute(args, file_io, nullptr);
}

json McpProxiedTool::execute(const json& args,
                             [[maybe_unused]] FileIO& file_io,
                             ToolProgressFn progress) {
    auto emit_progress = [&progress](const std::string& stage,
                                     int percent,
                                     const std::string& message) {
        if (!progress) return;
        json p = {
            {"stage",   stage},
            {"percent", percent},
            {"message", message},
        };
        progress(p);
    };

    // §Phase 11: uses injected mcp_manager_ instead of ::instance().
    // If mcp_manager_ is nullptr (shouldn't happen in practice), return error.
    if (!mcp_manager_) {
        return json::object({
            {"status",  "error"},
            {"server",  server_name_},
            {"tool",    def_.name},
            {"message",  "MCP manager not configured (mcp_enabled=false)"}
        });
    }
    // §Phase C2: emit "calling" before the MCP round-trip. This is the
    // stage that has the most latency variance — stdio servers spawn a
    // child process on first call (often 1-2s), HTTP servers do a
    // network round-trip, etc. Without this event the UI is frozen
    // until the result comes back.
    emit_progress("calling", 0,
                  "Calling " + server_name_ + "::" + def_.name);
    mcp::ToolResult result =
        mcp_manager_->call_tool(server_name_, def_.name, args);

    // §Phase C2: emit "parsing" once the server's response is back. The
    // 90% milestone is intentional — result shaping (content extraction,
    // error mapping) is fast but not free, and reserving the last 10%
    // for the implicit "done" signal that tool_result provides.
    emit_progress("parsing", 90, "Parsing response");

    json response = json::object({
        {"status", result.success ? "success" : "error"},
        {"server", server_name_},
        {"tool",   def_.name}
    });

    if (result.success) {
        response["result"] = result.content;
    } else {
        response["message"] = result.error.value_or("Unknown error");
    }

    return response;
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

std::string McpProxiedTool::sanitize_name(const std::string& s) {
    // OpenAI / Anthropic tool names allow letters, digits, underscores and
    // dashes. Anything else gets squashed to '_' so the namespace stays
    // parseable and the underlying routing is unambiguous.
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        const unsigned char uc = static_cast<unsigned char>(c);
        const bool ok =
            std::isalnum(uc) || c == '_' || c == '-';
        out.push_back(ok ? c : '_');
    }
    return out;
}