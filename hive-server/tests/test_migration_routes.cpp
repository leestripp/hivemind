// =============================================================================
// test_migration_routes — Phase 73: HTTP boundary for agent migration
// (Plan.md). Route-level validation matrix; the orchestration happy path
// needs a connected worker and is proven by the live Buzz migration (M1
// exit test). Worker channel is constructed but never start()ed — every
// worker reads as offline.
// =============================================================================

#include "hivemind/routes/MigrationRoutes.h"

#include "hivemind/AgentStore.h"
#include "hivemind/ChatService.h"
#include "hivemind/DeliveryStore.h"
#include "hivemind/HubRouter.h"
#include "hivemind/MigrationService.h"
#include "hivemind/MigrationStore.h"
#include "hivemind/RoutineStore.h"
#include "hivemind/SecretStore.h"
#include "hivemind/ThreadStore.h"
#include "hivemind/WorkerChannel.h"
#include "hivemind/WorkerStore.h"

#include <OmniNet.h>
#include <nlohmann/json.hpp>

#include <cassert>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <thread>

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {

OmniNet::HTTP::ClientResponse request(const std::string& method,
                                      uint16_t port,
                                      const std::string& path,
                                      const json& body = json()) {
    OmniNet::HTTP::ClientRequest req;
    req.method(method)
       .url("http://127.0.0.1:" + std::to_string(port) + path)
       .header("Content-Type", "application/json");
    if (!body.is_null()) req.body(body.dump());
    auto res = OmniNet::HTTP::Client::get_instance().send_blocking(req);
    assert(res && "request failed to reach server");
    return *res;
}

json jget(uint16_t port, const std::string& path) {
    return json::parse(request("GET", port, path).bodyAsString());
}

}  // namespace

int main() {
    fs::path tmp = fs::temp_directory_path() / "hivemind_test_migration_routes";
    fs::remove_all(tmp);
    fs::create_directories(tmp);

    AgentStore agents{tmp / "agents.db"};
    ThreadStore threads{tmp / "threads.db"};
    RoutineStore routines{tmp / "routines.db"};
    WorkerStore wstore{tmp / "workers.db"};
    WorkerChannel wchannel{wstore, 0, "127.0.0.1"};   // never start()ed
    hivemind::SecretStore secrets{tmp / "secrets.db", tmp / "secrets.key"};
    HubRouter hub{agents, threads, wchannel, routines, secrets};
    hivemind::DeliveryStore deliveries{tmp / "deliveries.db"};
    ChatService chat{agents, threads, hub, deliveries};
    hivemind::MigrationStore mstore{tmp / "migrations.db"};
    hivemind::MigrationService svc{mstore, agents, wchannel, hub};
    svc.set_chat_service(&chat);

    OmniNet::HTTP::Server::Config cfg;
    cfg.port = 0;  // ephemeral
    OmniNet::HTTP::Server server{cfg};
    register_migration_routes(server, svc);
    server.startAsync();
    for (int i = 0; i < 50 && server.boundPort() == 0; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    const uint16_t port = server.boundPort();
    assert(port != 0);

    // ── setup: one agent homed on an (offline) enrolled worker ─────────
    wstore.enroll("testrig");

    // ── validation matrix ──────────────────────────────────────────────
    // unknown agent → 404
    auto r = request("POST", port, "/api/agents/nope/migrate",
                     {{"target_worker", "testrig"}});
    assert(r.statusCode() == 404);

    // Roster routes aren't registered here — create agents store-direct.
    // (RosterRoutes already regression-covers worker validation.)
    const std::string local_id =
        agents.create_agent("Localo", "x", "", "shared", "", "local").id;
    const std::string buzz =
        agents.create_agent("Buzz", "news", "", "shared", "", "testrig").id;

    // agent on hub-side "local" → 400 (not migratable yet)
    r = request("POST", port, "/api/agents/" + local_id + "/migrate",
                {{"target_worker", "testrig"}});
    assert(r.statusCode() == 400);

    // offline target worker → 400
    r = request("POST", port, "/api/agents/" + buzz + "/migrate",
                {{"target_worker", "ghost-rig"}});
    assert(r.statusCode() == 400);

    // bogus target_type → 400
    r = request("POST", port, "/api/agents/" + buzz + "/migrate",
                {{"target_worker", "testrig"}, {"target_type", "vmware"}});
    assert(r.statusCode() == 400);

    // bad JSON → 400
    {
        OmniNet::HTTP::ClientRequest req;
        req.method("POST")
           .url("http://127.0.0.1:" + std::to_string(port) +
                "/api/agents/" + buzz + "/migrate")
           .header("Content-Type", "application/json")
           .body("{not json");
        auto res = OmniNet::HTTP::Client::get_instance().send_blocking(req);
        assert(res && res->statusCode() == 400);
    }

    // ── queries on an empty job table ──────────────────────────────────
    assert(jget(port, "/api/migrations")["migrations"].empty());
    r = request("GET", port, "/api/migrations/nope");
    assert(r.statusCode() == 404);
    r = request("POST", port, "/api/migrations/nope/cancel");
    assert(r.statusCode() == 400);

    std::cout << "test_migration_routes: OK\n";
    return 0;
}
