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
//   - hive-brain  (AI agent)  — gitlab.com/leestripp/hive-brain (CMake FetchContent)
//   - SQLite   (persistence) — system package
//   (no libcurl — hive-brain and hivemind both use OmniNet HTTP)
//   - nlohmann/json         — system package
// =============================================================================

#include "hivemind/Settings.h"
#include "hivemind/Schedule.h"
#include "hivemind/DbUtil.h"

#include "hivemind/routes/StaticRoutes.h"
#include "hivemind/routes/SystemRoutes.h"
#include "hivemind/SystemMonitor.h"
#include "hivemind/routes/RosterRoutes.h"
#include "hivemind/routes/FileRoutes.h"
#include "hivemind/routes/ConnectorRoutes.h"
#include "hivemind/ConnectorStore.h"
#include "hivemind/routes/ContainerRoutes.h"
#include "hivemind/routes/ApprovalRoutes.h"
#include "hivemind/routes/RoutineRoutes.h"
#include "hivemind/routes/SkillRoutes.h"
#include "hivemind/routes/SecretRoutes.h"
#include "hivemind/routes/ScreenRoutes.h"
#include "hivemind/routes/RouteUtil.h"
#include "hivemind/ServerConfig.h"
#include "hivemind/routes/KeyRoutes.h"
#include "hivemind/routes/ConfigRoutes.h"
#include "hivemind/AuthStore.h"
#include "hivemind/WorkerStore.h"
#include "hivemind/WorkerChannel.h"
#include "hivemind/WorkerRouter.h"
#include "hivemind/AgentTools.h"
#include "hivemind/routes/WorkerRoutes.h"
#include "hivemind/routes/ModelRoutes.h"
#include "hivemind/LlmStore.h"
#include "hivemind/SecretStore.h"
#include "hivemind/RoutineStore.h"
#include "hivemind/AgentStore.h"
#include "hivemind/ThreadStore.h"
#include "hivemind/ChatService.h"
#include "hivemind/DeliveryStore.h"
#include "hivemind/HubRouter.h"
#include "hivemind/ApprovalStore.h"

#include <OmniNet.h>
#include <nlohmann/json.hpp>

#include <atomic>
#include <ctime>
#include <chrono>
#include <csignal>
#include <iostream>
#include <thread>

static std::atomic<bool> g_shutdown{false};

static void on_signal(int) { g_shutdown = true; }

int main(int argc, char* argv[]) {
    // Phase 30.1 CLI: worker enrollment is admin-only, done on the server.
    //   hive-server enroll-worker <name>   → prints the raw hmwk_ key ONCE
    //   hive-server revoke-worker <name>
    if (argc >= 3 && std::string{argv[1]} == "enroll-worker") {
        try {
            WorkerStore ws;
            std::string raw = ws.enroll(argv[2]);
            std::cout << "worker '" << argv[2] << "' enrolled. Key (shown ONCE):\n"
                      << raw << "\n"
                      << "Give it to the worker: hive-worker --key <key>\n";
            return 0;
        } catch (const std::exception& e) {
            std::cerr << "enroll-worker: " << e.what() << "\n";
            return 1;
        }
    }
    if (argc >= 3 && std::string{argv[1]} == "revoke-worker") {
        WorkerStore ws;
        bool ok = ws.remove(argv[2]);
        std::cout << (ok ? "revoked\n" : "no such worker\n");
        return ok ? 0 : 1;
    }

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
    // 2026-09-17: OmniNet defaults maxRequestSize to 1 MB — the hub
    // dropped every upload over 1 MB at the socket (nginx saw a reset
    // and returned 502) once nginx's own 1 MB cap was raised. 16 MB
    // covers the 10 MB kMaxUpload (FileRoutes) with headroom.
    cfg.maxRequestSize = 16 * 1024 * 1024;

    OmniNet::HTTP::Server server{cfg};

    // ─── Application state ──────────────────────────────────────────

    // HiveMind 2.0 — roster / chat state (Plan.md Phase 1)
    // Resolve (and first-run seed) the shared company workspace now,
    // not lazily at first container create — /workspace/index.md must
    // exist before any agent container mounts it.
    ServerConfig::instance().workspace_root();

    AgentStore agent_store;
    ThreadStore thread_store;
    thread_store.ensure_user_thread();   // Phase 44: User Inbox always exists
    ApprovalStore approval_store;
    RoutineStore routine_store;
    // 2026-09-23 migration: routine threads were created as kind "dm",
    // which made find_dm() resolve an agent's user DM to the LAST-created
    // routine thread (the Android voice app and desktop roster clicks
    // landed in "routine: daily-team-checkin"). Re-tag them "routine" —
    // routines store their thread_id, so this is exact, no title guesswork.
    for (auto& r : routine_store.list())
        thread_store.set_kind(r.thread_id, "routine");
    AuthStore auth_store;
    WorkerStore worker_store;
    LlmStore llm_store;
    WorkerChannel worker_channel{worker_store,
                                 ServerConfig::instance().worker_ws_port(),
                                 ServerConfig::instance().worker_ws_bind()};
    WorkerRouter::install(&agent_store, &worker_channel);   // Phase 30.3
    // Phase 30: let the collaboration tools see the worker registry too
    // (list_workers tool + create_agent's worker placement param).
    agent_tools::install_worker_access(&worker_store, &worker_channel);
    // Phase C: pure hub. Turns run on workers (hive-agent); the HubRouter
    // dispatches task ops over the worker channel and feeds results back.
    hivemind::SecretStore secret_store;   // hoisted: hub handles secret_req
    HubRouter hub{agent_store, thread_store, worker_channel, approval_store,
                  routine_store, secret_store};
    hub.set_llm_store(&llm_store);
    // Phase 66b: durable delivery queue. Boot recovery — wakes left
    // 'dispatched' by the previous process go back to 'queued' and drain
    // as agents report ready; terminal rows prune after 7 days.
    hivemind::DeliveryStore delivery_store;
    {
        const int requeued = delivery_store.requeue_orphans();
        if (requeued > 0)
            std::fprintf(stderr,
                "[delivery] boot: %d orphaned wake(s) requeued\n", requeued);
        delivery_store.prune_terminal_older_than(7);
    }
    ChatService chat_service{agent_store, thread_store, hub,
                             delivery_store};
    hub.set_chat_service(&chat_service);
    worker_channel.on_frame =
        [&hub](const std::string& w, const nlohmann::json& f) {
            hub.on_worker_frame(w, f);
        };
    worker_channel.on_heartbeat =
        [&hub](const std::string& w, const nlohmann::json& agents) {
            hub.on_heartbeat(w, agents);
        };
    // Phase 33: on worker (re)connect, re-push every enabled global skill
    // for each agent homed on that rig (self-heals after worker downtime).
    worker_channel.on_worker_online = [&agent_store, &hub,
                                       &worker_channel](const std::string& wname) {
        // Phase C: spawn roster agents homed here (idempotent).
        hub.reconcile_worker(wname);
        json keep = json::array();
        for (const auto& a : agent_store.list_agents()) {
            if (a.worker != wname) continue;
            for (const auto& sk : agent_tools::list_enabled_skills(a.id)) {
                agent_tools::push_skill_to_worker(a, sk);
                keep.push_back(sk);
            }
        }
        // Prune stale skill dirs on the worker: the sync protocol only
        // adds, so leftovers from long-disabled skills would linger in
        // the shared /skills mount forever (hit 2026-09-03).
        worker_channel.send(wname,
                            {{"op","skills_prune"},{"keep",keep}});
    };
    ConnectorStore connector_store;

    // ─── Auth (Phase 9) ─────────────────────────────────────────────
    // First run prints the master key once; every /api/* route except
    // /api/health then requires `Authorization: Bearer <key>`.
    auth_store.ensure_master_key(/*print=*/true);

    // ─── CORS (Phase 9) ─────────────────────────────────────────────
    // Same-origin web UI needs nothing. For cross-origin API clients the
    // admin adds exact origins to server.json "cors_origins" (or "*").
    // Preflight is answered here (no auth required); actual responses get
    // their ACAO header from RouteUtil::send_json via the thread-local.
    server.use([](const OmniNet::HTTP::Request& req,
                  OmniNet::HTTP::Response& resp) -> bool {
        // Phase 19: per-request id for structured error notices.
        static std::atomic<unsigned long> seq{0};
        hivemind::routes::request_id =
            "req-" + std::to_string(++seq) + "-" +
            std::to_string(std::time(nullptr) & 0xffffff);
        auto origin = req.header("Origin");
        hivemind::routes::request_origin = origin ? std::string{*origin} : "";
        if (req.method() != OmniNet::HTTP::Method::OPTIONS ||
            !origin || !req.header("Access-Control-Request-Method"))
            return true;   // not a preflight
        resp = OmniNet::HTTP::Response::ok();
        resp.setStatus(OmniNet::HTTP::StatusCode::NO_CONTENT);
        if (ServerConfig::instance().cors_allowed(std::string{*origin})) {
            resp.setHeader("Access-Control-Allow-Origin", *origin);
            resp.setHeader("Access-Control-Allow-Methods",
                           "GET, POST, PUT, PATCH, DELETE, OPTIONS");
            resp.setHeader("Access-Control-Allow-Headers",
                           "Authorization, Content-Type");
            resp.setHeader("Access-Control-Max-Age", "86400");
            resp.setHeader("Vary", "Origin");
        }
        return false;      // preflight answered — no handler runs
    });

    // Global auth middleware — runs before every handler; guards /api/*.
    server.use([&auth_store](const OmniNet::HTTP::Request& req,
                             OmniNet::HTTP::Response& resp) -> bool {
        std::string path{req.path()};
        if (path.rfind("/api/", 0) != 0) return true;  // static UI, admin
        if (path == "/api/health") return true;       // probe stays open
        // Phase 23: the OAuth provider redirect carries no bearer — the
        // one-shot state token in the query IS the credential.
        if (path == "/api/oauth/callback") return true;

        auto deny = [&](int code, const std::string& msg) {
            hivemind::routes::send_error(resp, code, msg);
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
            path.rfind("/api/secrets", 0) == 0 ||
            // Model-registry management is credential-adjacent
            // (relay to worker models.json/secrets.json) — admin only.
            (path.rfind("/api/workers/", 0) == 0 &&
             (path.find("/models") != std::string::npos ||
              path.find("/secrets") != std::string::npos)) ||
            (path.rfind("/api/connectors", 0) == 0 &&
             req.method() != OmniNet::HTTP::Method::GET) ||
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
    // Phase 12: graph/workflow/trigger/run routes REMOVED — workflow engine
    // retired; /api/graphs/* and /api/hooks/* no longer exist.
    register_roster_routes(server, agent_store, thread_store,
                           hub, chat_service, routine_store, worker_store);
    register_file_routes(server, thread_store, agent_store,
                         &worker_channel);
    register_connector_routes(server, connector_store, secret_store);
    register_container_routes(server, agent_store);
    register_approval_routes(server, hub, approval_store, agent_store);
    register_key_routes(server, auth_store);
    register_worker_routes(server, worker_store, worker_channel);
    register_model_routes(server, llm_store);
    register_config_routes(server);
    register_routine_routes(server, routine_store, agent_store,
                            thread_store, chat_service);
    register_skill_routes(server, agent_store);
    register_secret_routes(server, agent_store, secret_store,
        [&hub](const std::string& agent_id) {
            // Grant changed → push a fresh spawn frame so the agent's env
            // picks it up. Deferred while the agent is mid-turn (Phase
            // 67.2 — an immediate respawn killed in-flight turns).
            hub.push_spawn(agent_id);
        });
    register_screen_routes(server, agent_store, auth_store);

    // 3. Static / web UI catch-all (must be LAST per OmniNet docs).
    static SystemMonitor system_monitor;
    system_monitor.start();
    register_system_routes(server, system_monitor, agent_store, thread_store);
    register_static_routes(server);

    // 4. Admin endpoints (dashboard at /omninet/*).
    try { server.adminHandler().mount(server); }
    catch (...) { /* admin not always available; ignore */ }

    // ─── Run ────────────────────────────────────────────────────────
    // BOTH start()s block in accept loops — one jthread each.
    std::jthread worker_ws_thread{[&worker_channel]() {
        worker_channel.start();   // Phase 30.2: hive-worker WS channel
    }};
    std::jthread server_thread{[&server]() { server.start(); }};

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
    std::jthread scheduler{[&routine_store, &chat_service,
                            &thread_store, &agent_store,
                            &worker_channel](std::stop_token st) {
        while (!st.stop_requested()) {
            for (int i = 0; i < 30 && !st.stop_requested(); ++i)
                std::this_thread::sleep_for(std::chrono::seconds(1));
            if (st.stop_requested()) break;
            // ── Agent routines (Phase 5): fire due routine prompts into
            // their threads. Per-routine guard mirrors the trigger policy.
            try {
                const std::string now = hm::now_iso_local();
                for (auto& r : routine_store.list_due(now)) {
                    try {
                        json sched = json::parse(r.schedule_json, nullptr, false);
                        const bool once = sched.is_object()
                            && sched.value("type", "") == "once";
                        // Phase 17 skip-if-running + 2026-10-01 offline
                        // void: never pile a routine prompt into a thread
                        // with a turn in flight, and NEVER fire into an
                        // offline worker — an offline dispatch would leave
                        // a durable QUEUED delivery row per tick that all
                        // drain at once on reconnect. Recurring fires on
                        // busy/offline targets are VOIDED (next_fire_at
                        // advances, nothing queued, nothing dispatched).
                        if (!agent_store.get_agent(r.agent_id)) {
                            routine_store.mark_fired(r.id, now, "", true);
                            std::cout << "[scheduler] routine " << r.id
                                      << " (" << r.name
                                      << ") agent gone, disabling\n";
                            continue;
                        }
                        const bool online = worker_channel.is_online(
                            agent_store.get_agent(r.agent_id)->worker);
                        switch (hm::routine_fire_action(
                                sched, chat_service.busy(r.thread_id),
                                online)) {
                        case hm::RoutineFireAction::RetryLater:
                            std::cout << "[scheduler] routine " << r.id
                                      << " (" << r.name << ")"
                                      << (online ? " busy," : " offline,")
                                      << " retry next tick\n";
                            continue;
                        case hm::RoutineFireAction::SkipReschedule: {
                            auto next = hm::compute_next_fire(sched, now, r.created_at);
                            routine_store.reschedule(r.id, next.value_or(""), !next);
                            std::cout << "[scheduler] routine " << r.id
                                      << " (" << r.name << ")"
                                      << (online ? " busy," : " offline,")
                                      << " fire VOIDED (nothing queued)\n";
                            continue;
                        }
                        case hm::RoutineFireAction::Fire:
                            break;
                        }
                        // Self-heal: the owned thread may have been
                        // deleted (2026-09-16 incident) — recreate and
                        // rebind instead of failing into disable.
                        if (!thread_store.get_thread(r.thread_id)) {
                            auto t = thread_store.create_thread(
                                r.name, "routine", {r.agent_id});
                            routine_store.rebind_thread(r.id, t.id);
                            r.thread_id = t.id;
                            std::cout << "[scheduler] rebound routine "
                                      << r.id << " to fresh thread "
                                      << t.id << "\n";
                        }
                        chat_service.post_user_message(r.thread_id, r.prompt);
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
            // ── Proactive nudges (gap analysis): a user message that sat
            // unanswered for 15+ minutes means the turn was dropped (LLM
            // error, crash, restart). Post ONE system nudge — the nudge
            // itself becomes the latest message, so the thread drops out
            // of the query until the user speaks again. No state table.
            try {
                for (auto& t : thread_store.threads_awaiting_agent(
                         hivemind::db::iso_ago(15 * 60))) {
                    if (chat_service.busy(t.id)) continue;   // turn in flight
                    const std::string msg =
                        "[nudge] The user message above has been waiting "
                        "over 15 minutes without a reply. If your previous "
                        "turn was interrupted, please continue now; "
                        "otherwise briefly acknowledge.";
                    chat_service.post_nudge(t.id, msg);
                    std::cout << "[scheduler] nudged thread " << t.id
                              << " (" << t.title << ")\n";
                }
            } catch (const std::exception& e) {
                std::cerr << "[scheduler] nudge tick failed: " << e.what() << "\n";
            }
        }
    }};

    std::cout << "Scheduler running (30s tick, server-local time).\n";
    std::cout << "Press Ctrl-C to stop.\n";

    while (!g_shutdown.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    std::cout << "\nShutting down...\n";
    // Stop chat first: no new turns, and in-flight turn threads finish
    // their final message writes before the stores go away (Phase 10).
    chat_service.quiesce();
    // Agents live on workers in v2: history persists in their containers;
    // no server-side shutdown persistence.
    worker_channel.stop();
    server.stop();
    OmniNet::cleanup();
    return 0;
}
