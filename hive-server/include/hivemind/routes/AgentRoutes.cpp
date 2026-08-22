// =============================================================================
// include/hivemind/routes/AgentRoutes.cpp
// -----------------------------------------------------------------------------
// /api/agent/tools and /api/agent/status — introspection endpoints for
// the embedded AI Agent engine. They construct a throwaway
// daedalus::Agent instance from the default model config (so they
// reflect whatever tools that model has configured) and call its
// list_tools() / state() / stats() accessors.
// =============================================================================

#include "hivemind/routes/AgentRoutes.h"

#include <daedalus.h>

#include <OmniNet.h>
#include <nlohmann/json.hpp>

#include <filesystem>
#include <memory>
#include <mutex>

using json = nlohmann::json;
namespace fs = std::filesystem;

namespace {

// Build a minimal AgentConfig from the default model. Returns nullopt
// if no model is configured (the server is running but no LLM is
// reachable yet). The two endpoints handle nullopt by returning
// empty stubs instead of 500-ing.
// Minimal config for introspection endpoints. Model is left unset —
// daedalus resolves the default from its own SettingsStore.
daedalus::AgentConfig introspection_agent_config() {
    daedalus::AgentConfig cfg;
    cfg.session_id   = "hivemind-introspection";
    cfg.project_root = fs::current_path().string();
    cfg.auto_approve = true;
    return cfg;
}

// daedalus::Agent is not thread-safe, and constructing several agents
// concurrently races on shared global state (SettingsStore, curl globals,
// MCP config) — observed as a SIGSEGV when the browser fired /tools,
// /skills and /mcp simultaneously. Serialize all introspection endpoints.
std::mutex g_introspection_mtx;

}  // namespace

void register_agent_routes(OmniNet::HTTP::Server& server) {

    // GET /api/agent/tools — list all available tools
    server.get("/api/agent/tools",
        [](const OmniNet::HTTP::Request& req, OmniNet::HTTP::Response& resp) {
            (void)req;
            std::lock_guard<std::mutex> lk{g_introspection_mtx};
            try {
                // Throwaway sink — tool listing doesn't fire callbacks.
                struct NullSink : daedalus::AgentSink {};
                daedalus::Agent agent{introspection_agent_config(), std::make_shared<NullSink>()};
                agent.initialize();
                auto tools = agent.list_tools();

                json arr = json::array();
                for (auto& t : tools) {
                    arr.push_back({
                        {"name", std::string(t.name)},
                        {"description", std::string(t.description)},
                    });
                }
                OmniNet::HTTP::Response r = OmniNet::HTTP::Response::ok();
                r.setContentType("application/json");
                r.setBody(arr.dump());
                resp = r;
            } catch (const std::exception& e) {
                OmniNet::HTTP::Response r = OmniNet::HTTP::Response::internalError(e.what());
                r.setContentType("application/json");
                r.setBody(json{{"error", e.what()}}.dump());
                resp = r;
            }
        });

    // GET /api/agent/status — agent system info
    server.get("/api/agent/status",
        [](const OmniNet::HTTP::Request& req, OmniNet::HTTP::Response& resp) {
            (void)req;
            std::lock_guard<std::mutex> lk{g_introspection_mtx};
            try {
                struct NullSink : daedalus::AgentSink {};
                daedalus::Agent agent{introspection_agent_config(), std::make_shared<NullSink>()};
                agent.initialize();
                auto tools = agent.list_tools();
                auto stats = agent.stats();

                json arr = json::array();
                for (auto& t : tools) {
                    arr.push_back({
                        {"name", std::string(t.name)},
                        {"description", std::string(t.description)},
                    });
                }
                OmniNet::HTTP::Response r = OmniNet::HTTP::Response::ok();
                r.setContentType("application/json");
                r.setBody(json{
                    {"status",     "ok"},
                    {"tool_count", tools.size()},
                    {"tools",      arr},
                    {"stats",      {
                        {"prompt_tokens",     stats.prompt_tokens},
                        {"completion_tokens", stats.completion_tokens},
                        {"total_tokens",      stats.total_tokens},
                    }},
                }.dump());
                resp = r;
            } catch (const std::exception& e) {
                OmniNet::HTTP::Response r = OmniNet::HTTP::Response::internalError(e.what());
                r.setContentType("application/json");
                r.setBody(json{{"error", e.what()}}.dump());
                resp = r;
            }
        });

    // GET /api/agent/skills — list discoverable skills
    server.get("/api/agent/skills",
        [](const OmniNet::HTTP::Request& req, OmniNet::HTTP::Response& resp) {
            (void)req;
            std::lock_guard<std::mutex> lk{g_introspection_mtx};
            try {
                struct NullSink : daedalus::AgentSink {};
                daedalus::Agent agent{introspection_agent_config(), std::make_shared<NullSink>()};
                agent.initialize();
                auto skills = agent.list_skills();

                json arr = json::array();
                for (auto& sk : skills) {
                    arr.push_back({
                        {"name",        sk.name},
                        {"description", sk.description},
                        {"tags",        sk.tags},
                    });
                }
                OmniNet::HTTP::Response r = OmniNet::HTTP::Response::ok();
                r.setContentType("application/json");
                r.setBody(arr.dump());
                resp = r;
            } catch (const std::exception& e) {
                OmniNet::HTTP::Response r = OmniNet::HTTP::Response::internalError(e.what());
                r.setContentType("application/json");
                r.setBody(json{{"error", e.what()}}.dump());
                resp = r;
            }
        });

    // GET /api/agent/mcp — list configured MCP servers
    server.get("/api/agent/mcp",
        [](const OmniNet::HTTP::Request& req, OmniNet::HTTP::Response& resp) {
            (void)req;
            std::lock_guard<std::mutex> lk{g_introspection_mtx};
            try {
                struct NullSink : daedalus::AgentSink {};
                daedalus::Agent agent{introspection_agent_config(), std::make_shared<NullSink>()};
                agent.initialize();
                auto servers = agent.mcp_configured_servers();

                json arr = json::array();
                for (auto& name : servers) arr.push_back(name);
                OmniNet::HTTP::Response r = OmniNet::HTTP::Response::ok();
                r.setContentType("application/json");
                r.setBody(arr.dump());
                resp = r;
            } catch (const std::exception& e) {
                OmniNet::HTTP::Response r = OmniNet::HTTP::Response::internalError(e.what());
                r.setContentType("application/json");
                r.setBody(json{{"error", e.what()}}.dump());
                resp = r;
            }
        });
}
