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

#include "hivemind/routes/GraphRoutes.h"
#include "hivemind/routes/RunRoutes.h"
#include "hivemind/routes/StaticRoutes.h"
#include "hivemind/routes/ModelRoutes.h"
#include "hivemind/routes/ProjectRoutes.h"
#include "hivemind/routes/AgentRoutes.h"

#include <OmniNet.h>

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
    register_run_routes(server, store, registry);

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
    std::cout << "Press Ctrl-C to stop.\n";

    while (!g_shutdown.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    std::cout << "\nShutting down...\n";
    server.stop();
    OmniNet::cleanup();
    return 0;
}
