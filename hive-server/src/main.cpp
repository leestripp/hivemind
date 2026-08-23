// =============================================================================
// src/main.cpp
// -----------------------------------------------------------------------------
// HiveMind Node Graph — server entry point.
//
// Architecture (per docs/design.md):
//   - The HTTP API and the web UI are served on the same port via a
//     single OmniNet::HTTP::Server.
//   - All API routes are registered FIRST (per OmniNet docs/api/routes.md
//     — first-match-wins routing; catch-all static handler must come last).
//   - The web UI's static handler is registered LAST as the catch-all.
//
// Library stack:
//   - OmniNet (HTTP + WS) — gitlab.com/leestripp/omninet (CMake FetchContent)
//   - daedalus  (AI agent)  — gitlab.com/leestripp/daedalus (CMake FetchContent)
//   - SQLite   (persistence) — system package
//   - libcurl  (LLM HTTP)   — system package
//   - nlohmann/json         — system package
// =============================================================================

#include "hivemind/Settings.h"
#include "hivemind/GraphStore.h"
#include "hivemind/ExecutorRegistry.h"
#include "hivemind/Schedule.h"

#include "hivemind/routes/GraphRoutes.h"
#include "hivemind/routes/RunRoutes.h"
#include "hivemind/routes/WsRunRoutes.h"
#include "hivemind/routes/TriggerRoutes.h"
#include "hivemind/routes/StaticRoutes.h"
#include "hivemind/routes/ModelRoutes.h"
#include "hivemind/routes/ProjectRoutes.h"
#include "hivemind/routes/AgentRoutes.h"
#include "hivemind/routes/RosterRoutes.h"
#include "hivemind/routes/ContainerRoutes.h"
#include "hivemind/routes/ApprovalRoutes.h"
#include "hivemind/routes/RoutineRoutes.h"
#include "hivemind/routes/KeyRoutes.h"
#include "hivemind/routes/ConfigRoutes.h"
#include "hivemind/AuthStore.h"
#include "hivemind/RoutineStore.h"
#include "hivemind/AgentStore.h"
#include "hivemind/ThreadStore.h"
#include "hivemind/AgentHost.h"
#include "hivemind/ApprovalStore.h"

#include <OmniNet.h>
#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <csignal>
#include <iostream>
#include <thread>

static std::atomic<bool> g_shutdown{false};

static void on_signal(int) { g_shutdown = true; }

int main(int argc, char* argv[]) {
    auto settings = load_settings(argc, argv);

    std::cout << "HiveMind Node Graph v"
              << (sizeof(void*) == 8 ? "0.0.0" : "0.0.0")
              << " starting on " << settings.address << ":" << settings.port << "\n";

    std::signal(SIGINT,  on_signal);
    std::signal(SIGTERM, on_signal);

    OmniNet::init();

    // ─── Server config ──────────────────────────────────────────────
    OmniNet::HTTP::Server::Config cfg;
    cfg.address       = settings.address;
    cfg.port          = settings.port;
    cfg.corsEnabled   = true;
    cfg.corsAllowedOrigins = "*";
    cfg.logRequests   = false;
    cfg.requestTimeout = std::chrono::seconds(60);
    cfg.workerCount    = 4;

    OmniNet::HTTP::Server server{cfg};

    // ─── Application state ──────────────────────────────────────────
    GraphStore store;
    ExecutorRegistry registry;

    // HiveMind 2.0 — roster / chat state (Plan.md Phase 1)
    AgentStore agent_store;
    ThreadStore thread_store;
    ApprovalStore approval_store;
    RoutineStore routine_store;
    AuthStore auth_store;
    AgentRegistry agent_registry{agent_store, approval_store};
    ChatService chat_service{agent_store, thread_store, agent_registry};

    // ─── Auth (Phase 9) ─────────────────────────────────────────────
    // First run prints the master key once; every /api/* route except
    // /api/health then requires `Authorization: Bearer <key>`.
    auth_store.ensure_master_key(/*print=*/true);

    // Global auth middleware — runs before every handler; guards /api/*.
    server.use([&auth_store](const OmniNet::HTTP::Request& req,
                             OmniNet::HTTP::Response& resp) -> bool {
        std::string path{req.path()};
        if (path.rfind("/api/", 0) != 0) return true;  // static UI, admin
        if (path == "/api/health") return true;       // probe stays open

        auto deny = [&](int code, const std::string& msg) {
            resp = OmniNet::HTTP::Response::ok();
            resp.setStatus(static_cast<OmniNet::HTTP::StatusCode>(code));
            resp.setContentType("application/json");
            resp.setBody("{\"error\":\"" + msg + "\"}");
            return false;
        };

        auto hdr = req.header("Authorization");
        if (!hdr || hdr->substr(0, 7) != "Bearer ")
            return deny(401, "missing bearer token");
        auto scopes = auth_store.validate(std::string{hdr->substr(7)});
        if (!scopes) return deny(401, "invalid or revoked api key");

        // Scope policy: admin-only routes, then read-only keys.
        const bool admin = AuthStore::has_scope(*scopes, "admin");
        if (path.rfind("/api/keys", 0) == 0 ||
            path.find("/container/exec") != std::string::npos ||
            (path == "/api/config" &&
             req.method() != OmniNet::HTTP::Method::GET)) {
            if (!admin) return deny(403, "admin scope required");
        }
        if (!admin && AuthStore::has_scope(*scopes, "read") &&
            !AuthStore::has_scope(*scopes, "agent")) {
            if (req.method() != OmniNet::HTTP::Method::GET)
                return deny(403, "read-only key");
        }
        return true;
    });

    // ─── Routes (REGISTERED IN ORDER — first match wins) ────────────
    // 1. Health probe (most specific, no params)
    server.get("/api/health", [](const OmniNet::HTTP::Request& req,
                                OmniNet::HTTP::Response& resp) {
        (void)req;
        resp.setStatus(OmniNet::HTTP::StatusCode::OK);
        resp.setContentType("application/json");
        resp.setBody(R"({"status":"ok","service":"hivemind"})");
    });

    // 2. Domain routes (all registered before the static catch-all).
    register_model_routes(server);
    register_project_routes(server);
    register_agent_routes(server);
    register_graph_routes(server, store);
    register_workflow_routes(server, store);
    register_trigger_routes(server, store, registry);
    register_run_routes(server, store, registry);
    register_ws_run_routes(server);
    register_roster_routes(server, agent_store, thread_store,
                           agent_registry, chat_service);
    register_container_routes(server, agent_store);
    register_approval_routes(server, approval_store);
    register_key_routes(server, auth_store);
    register_config_routes(server);
    register_routine_routes(server, routine_store, agent_store,
                            thread_store, chat_service);

    // 3. Static / web UI catch-all (must be LAST per OmniNet docs).
    register_static_routes(server);

    // 4. Admin endpoints (dashboard at /omninet/*).
    try { server.adminHandler().mount(server); }
    catch (...) { /* admin not always available; ignore */ }

    // ─── Run ────────────────────────────────────────────────────────
    std::jthread server_thread{[&server]() {
        server.start();
    }};

    std::cout << "HiveMind listening on http://" << settings.address
              << ":" << settings.port << "\n";
    std::cout << "API:        http://" << settings.address << ":"
              << settings.port << "/api/health\n";
    std::cout << "Web UI:     http://" << settings.address << ":"
              << settings.port << "/\n";
    // ─── Scheduled-trigger engine ─────────────────────────────────
    // Every 30s: fire enabled schedule triggers whose next_fire_at <= now
    // (server-local time), then recompute their next fire. Once-triggers
    // disable themselves after firing. The actual run goes through the
    // same start_graph_run() path as the REST Run button and webhooks.
    std::jthread scheduler{[&store, &registry, &routine_store,
                            &chat_service](std::stop_token st) {
        while (!st.stop_requested()) {
            for (int i = 0; i < 30 && !st.stop_requested(); ++i)
                std::this_thread::sleep_for(std::chrono::seconds(1));
            if (st.stop_requested()) break;
            try {
                const std::string now = hm::now_iso_local();
                for (auto& t : store.list_due_schedules(now)) {
                    // Per-trigger guard: a failing trigger must not abort the
                    // tick for the others, nor stay due and re-fire / spam
                    // the log every 30s forever — disable it on failure.
                    try {
                        auto g = store.get_graph(t.graph_id);
                        if (!g) {
                            std::cerr << "[scheduler] trigger " << t.id
                                      << ": graph gone, disabling\n";
                            store.mark_trigger_fired(t.id, now, "", true);
                            continue;
                        }
                        json cfg = json::parse(t.config_json, nullptr, false);
                        std::unordered_map<std::string, std::string> inputs;
                        if (cfg.is_object() && cfg.contains("inputs") && cfg["inputs"].is_object())
                            for (auto& [k, v] : cfg["inputs"].items())
                                if (v.is_string()) inputs[k] = v.get<std::string>();

                        std::string run_id = start_graph_run(store, registry, std::move(*g),
                                                             std::move(inputs));
                        const bool once = cfg.is_object() && cfg.contains("schedule")
                            && cfg["schedule"].is_object()
                            && cfg["schedule"].value("type", "") == "once";
                        auto next = once ? std::nullopt
                            : hm::compute_next_fire(cfg.value("schedule", json::object()),
                                                    now, t.created_at);
                        store.mark_trigger_fired(t.id, now,
                                                 next.value_or(""), once || !next);
                        std::cout << "[scheduler] fired trigger " << t.id
                                  << " (graph " << t.graph_id << ") → run " << run_id << "\n";
                    } catch (const std::exception& e) {
                        std::cerr << "[scheduler] trigger " << t.id
                                  << " failed, disabling: " << e.what() << "\n";
                        try { store.mark_trigger_fired(t.id, now, "", true); }
                        catch (...) {}
                    }
                }
            } catch (const std::exception& e) {
                std::cerr << "[scheduler] tick failed: " << e.what() << "\n";
            }

            // ── Agent routines (Phase 5): fire due routine prompts into
            // their threads. Per-routine guard mirrors the trigger policy.
            try {
                const std::string now = hm::now_iso_local();
                for (auto& r : routine_store.list_due(now)) {
                    try {
                        chat_service.post_user_message(r.thread_id, r.prompt);
                        json sched = json::parse(r.schedule_json, nullptr, false);
                        const bool once = sched.is_object()
                            && sched.value("type", "") == "once";
                        auto next = once ? std::nullopt
                            : hm::compute_next_fire(sched, now, r.created_at);
                        routine_store.mark_fired(r.id, now,
                                                 next.value_or(""),
                                                 once || !next);
                        std::cout << "[scheduler] fired routine " << r.id
                                  << " (" << r.name << ")\n";
                    } catch (const std::exception& e) {
                        std::cerr << "[scheduler] routine " << r.id
                                  << " failed, disabling: " << e.what() << "\n";
                        try { routine_store.mark_fired(r.id, now, "", true); }
                        catch (...) {}
                    }
                }
            } catch (const std::exception& e) {
                std::cerr << "[scheduler] routine tick failed: " << e.what() << "\n";
            }
        }
    }};

    std::cout << "Scheduler running (30s tick, server-local time).\n";
    std::cout << "Press Ctrl-C to stop.\n";

    while (!g_shutdown.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    std::cout << "\nShutting down...\n";
    // Persist roster-agent conversation history (daedalus only saves on
    // Agent::shutdown — its destructor doesn't).
    agent_registry.shutdown_all();
    server.stop();
    OmniNet::cleanup();
    return 0;
}
