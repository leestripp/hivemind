#include "tools/ToolFactory.h"
#include "tools/McpProxiedTool.h"
#include "tools/UserTool.h"
#include "tools/SetStatusTool.h"
#include "tools/BashTool.h"
#include "tools/SkillTool.h"
#include "tools/MemoryTool.h"
#include "tools/ReadImageTool.h"
#include "tools/WebFetchTool.h"
#include "tools/WebSearchTool.h"
#include "tools/SearxngSearchTool.h"
#include "tools/PDFTool.h"
#include "tools/TodoListTool.h"
#include "tools/ListToolsTool.h"
#include "mcp/McpServerManager.h"
#include "core/FileIO.h"
#include "core/ThreadPool.h"
#include <iostream>
#include <algorithm>
#include <atomic>
#include <future>
#include <thread>
#include <vector>
ToolFactory::ToolFactory() {
    initialize_tools();
}

// §Phase 11: injected McpServerManager* (nullptr = MCP disabled).
void ToolFactory::set_mcp_manager(mcp::McpServerManager* mcp_manager) {
    mcp_manager_ = mcp_manager;
}

void ToolFactory::register_tool(std::shared_ptr<BaseTool> tool) {
    if (!tool) return;
    // §fix (2026-08-01): replace-by-name — a re-registered name updates the
    // existing slot instead of appending a duplicate definition (which
    // strict providers reject with HTTP 400 "function name is duplicated").
    const std::string name = tool->get_name();
    for (auto& existing : m_tools) {
        if (existing->get_name() == name) {
            existing = std::move(tool);
            return;
        }
    }
    m_tools.push_back(std::move(tool));
}

bool ToolFactory::unregister_tool(const std::string& tool_name) {
    for (auto it = m_tools.begin(); it != m_tools.end(); ++it) {
        if ((*it)->get_name() == tool_name) {
            m_tools.erase(it);
            return true;
        }
    }
    return false;
}
void ToolFactory::initialize_tools() {
    // §TOOL-TRIM (prompt.txt): the default tool surface is intentionally
    // minimal. Only the tools required for a functional agent are
    // registered here. File / git / grep/wiki-search / system-info /
    // background-process / network tools live in demos/example-tools/
    // and are registered by hosts that need them.
    // §PDF-CORE (2026-08-31): read_pdf moved back into core -- poppler-cpp
    // is a REQUIRED build dependency of the library, so the tool ships as
    // a built-in rather than an opt-in example.

    // User interaction tools
    register_tool(std::make_shared<TellUserTool>());
    register_tool(std::make_shared<StopTool>());

    // §COMPRESS-TOOL (2026-08-14): model-facing compression trigger,
    // restored. The tool only schedules compression — LLMClient turns the
    // result into the same compress_requested_ flag as
    // Agent::compress_context(), consumed at the cycle boundary.
    register_tool(std::make_shared<CompressContextTool>());

    // §State Machine: agent-driven state transitions.
    register_tool(std::make_shared<SetStatusTool>());

    // §LIST-TOOLS (2026-09-20): ground-truth introspection — models asked
    // "what tools do you have?" answer from (possibly stale) conversation
    // history otherwise.
    register_tool(std::make_shared<ListToolsTool>());

    // Bash tool
    register_tool(std::make_shared<BashTool>());

    // Skill tools
    register_tool(std::make_shared<DiscoverSkillsTool>());
    register_tool(std::make_shared<LoadSkillTool>());
    register_tool(std::make_shared<GetActiveSkillsTool>());
    register_tool(std::make_shared<SearchSkillsTool>());
    register_tool(std::make_shared<DeactivateSkillTool>());
    register_tool(std::make_shared<GetSkillContextTool>());

    // Memory tools
    register_tool(std::make_shared<RememberTool>());
    register_tool(std::make_shared<ForgetTool>());
    register_tool(std::make_shared<MemoriesTool>());
    register_tool(std::make_shared<MemoryStatsTool>());

    // §READ-IMAGE-CORE (2026-08-10): local image -> base64 data URI so
    // vision-capable models (Kimi K3, moonshot-v1-*-vision) can see it.
    // LLMClient carries the data URI into the next request as an
    // image_url content part and strips it from stored history, so the
    // tool is only useful when registered by default. Returns a
    // descriptive error for unsupported formats (e.g. SVG).
    register_tool(std::make_shared<ReadImageTool>());

    // §PDF-CORE: extract text from PDF files via poppler-cpp (required dep).
    register_tool(std::make_shared<ReadPDFTool>());

    // §WEB-CORE (2026-09-09): key-less web tools restored — these lived in
    // MonkeyByte and never made the daedalus in-tree merge (Phase 42/43),
    // leaving hive-agents with no web access after serper_search was
    // retired. DuckDuckGo HTML endpoint, no API key required.
    register_tool(std::make_shared<WebSearchTool>());
    register_tool(std::make_shared<WebFetchTool>());

    // §SEARXNG-CORE (2026-09-21): preferred search — our self-hosted
    // SearXNG metasearch on the leestrippai.com droplet (nginx :8443,
    // HTTP Basic). Credentials arrive as a hub-granted login secret named
    // "searxng" (HIVE_SECRET_SEARXNG_{USER,PASS,URL}, Phase 60); the tool
    // reports a descriptive error until the grant exists. web_search
    // (DuckDuckGo) stays as the credential-free fallback.
    register_tool(std::make_shared<SearxngSearchTool>());

    // Todo list tools
    register_tool(std::make_shared<TodoAddTool>());
    register_tool(std::make_shared<TodoEditTool>());
    register_tool(std::make_shared<TodoDeleteTool>());
    register_tool(std::make_shared<TodoListTool>());
    register_tool(std::make_shared<TodoSetMaxTool>());
    register_tool(std::make_shared<TodoCountTool>());
    register_tool(std::make_shared<TodoClearTool>());

    // MCP tools are NOT registered statically. They are populated dynamically
    // by `refresh_mcp_tools()` (called from LLMClient) so each server's
    // `tools/list` becomes one `McpProxiedTool` named `mcp__<server>__<tool>`.
    // See Agent.md §18.
}
json ToolFactory::get_all_definitions() const {
    json definitions = json::array();
    for (const auto& tool : m_tools) {
        definitions.push_back(tool->get_definition());
    }
    return definitions;
}

json ToolFactory::get_all_definitions(const ToolFilter& filter) const {
    return filter.filter_definitions(get_all_definitions());
}
json ToolFactory::execute_tool(const std::string& tool_name, const json& args, FileIO& file_io) {
    return execute_tool(tool_name, args, file_io, nullptr);
}

json ToolFactory::execute_tool(const std::string& tool_name,
                               const json& args,
                               FileIO& file_io,
                               ToolProgressFn progress) {
    for (const auto& tool : m_tools) {
        if (tool->get_name() == tool_name) {
            return tool->execute(args, file_io, progress);
        }
    }
    return json::object({{"status", "error"}, {"message", "Unknown tool: " + tool_name}});
}

json ToolFactory::execute_tool(const std::string& tool_name,
                               const json& args,
                               FileIO& file_io,
                               ToolProgressFn progress,
                               const ToolFilter& filter) {
    if (!filter.is_tool_allowed(tool_name)) {
        return json::object({
            {"status", "error"},
            {"message", "Tool '" + tool_name + "' is disabled by host policy"}
        });
    }
    // §LIST-TOOLS (2026-09-20): hand the per-agent filter to tools that
    // ask for it (FilterAwareTool) so e.g. list_tools answers reflect what
    // THIS agent may call. Per-call injection, no process-global state —
    // the factory is shared across Agent instances.
    if (auto tool = get_tool(tool_name)) {
        if (auto* aware = dynamic_cast<FilterAwareTool*>(tool.get())) {
            aware->set_active_filter(&filter);
        }
        return tool->execute(args, file_io, progress);
    }
    return json::object({{"status", "error"},
                         {"message", "Unknown tool: " + tool_name}});
}

void ToolFactory::refresh_mcp_tools() {
    // 1) Drop every previously-registered McpProxiedTool.
    m_tools.erase(
        std::remove_if(m_tools.begin(), m_tools.end(),
            [](const std::shared_ptr<BaseTool>& t) {
                return std::dynamic_pointer_cast<McpProxiedTool>(t) != nullptr;
            }),
        m_tools.end());

    // §Phase 11: guard with mcp_manager_ (nullptr when mcp_enabled=false).
    // Previously called ::instance() — now uses the injected pointer.
    if (!mcp_manager_) return;
    auto& mgr = *mcp_manager_;
    if (!mgr.is_config_loaded()) {
        return;
    }

    // Snapshot the configured-servers list because the manager's internal
    // connection state may change concurrently with this walk (HTTP
    // servers, list_changed handler).
    const auto configured = mgr.list_configured_servers();

    using hive::concurrent::ThreadPool;
    using hive::concurrent::TaskPriority;

    static constexpr size_t kMaxParallelConnects = 5;
    std::vector<std::future<bool>> futures;
    futures.reserve(configured.size());
    std::atomic<size_t> active{0};

    for (const auto& server_name : configured) {
        // Already connected? Skip the spawn — it's expensive.
        if (mgr.is_connected(server_name)) continue;

        // Throttle: at most 5 concurrent connects.
        // We spin-wait — these are short, and there's no async-friendly
        // semaphore in the pool yet.
        while (active.load() >= kMaxParallelConnects) {
            std::this_thread::yield();
        }
        ++active;

        futures.push_back(ThreadPool::get_instance().submit(
            TaskPriority::NORMAL,
            [&mgr, server_name, &active]() -> bool {
                bool ok = mgr.connect(server_name);
                --active;
                return ok;
            }));
    }
    // Wait for all to finish (success or breaker-recorded failure).
    for (auto& f : futures) {
        try { f.get(); } catch (...) {
            // Manager catches its own exceptions and updates the breaker.
        }
    }

    // 3) For every server that now reports healthy, register one
    // McpProxiedTool per discovered tool. We re-read list_configured_
    // servers() at this point in case the manager mutated it (e.g. list_
    // changed happened during connect).
    json schema_dumped = json::array();
    for (const auto& server_name : mgr.list_configured_servers()) {
        if (!mgr.is_healthy(server_name)) continue;
        const auto& tools = mgr.get_tools(server_name);
        for (const auto& def : tools) {
            // §Phase 11: pass mcp_manager_ to each McpProxiedTool
            // (injected instead of ::instance()).
            auto tool = std::make_shared<McpProxiedTool>(
                server_name, def, mcp_manager_);
            schema_dumped.push_back(tool->get_definition());
            register_tool(std::move(tool));
        }
    }

    // 4) §9.7.C — publish the schema snapshot so callers (LLMClient,
    // /mcp-tools command, token-warning HUD) can measure.
    mgr.set_schema_dumped_for_test(schema_dumped.dump());
    mgr.clear_mcp_tools_dirty();
}
bool ToolFactory::has_tool(const std::string& tool_name) const {
    for (const auto& tool : m_tools) {
        if (tool->get_name() == tool_name) {
            return true;
        }
    }
    return false;
}
std::shared_ptr<BaseTool> ToolFactory::get_tool(const std::string& tool_name) const {
    for (const auto& tool : m_tools) {
        if (tool->get_name() == tool_name) {
            return tool;
        }
    }
    return nullptr;
}
std::vector<std::string> ToolFactory::get_tool_names() const {
    std::vector<std::string> names;
    for (const auto& tool : m_tools) {
        names.push_back(tool->get_name());
    }
    return names;
}
