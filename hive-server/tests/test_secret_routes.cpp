// tests/test_secret_routes.cpp — grant-change spawn-push callback
// (2026-09-29 vault-loss fix): grant/revoke toggles and secret deletes
// must fire the GrantsChangedFn so the hub can push fresh spawn frames —
// grant writes are DB-only, and a running agent's HIVE_SECRET_* env only
// changes via a spawn frame.
#include "hivemind/routes/SecretRoutes.h"
#include "hivemind/AgentStore.h"
#include "hivemind/SecretStore.h"

#include <OmniNet.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cassert>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

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

uint16_t start(OmniNet::HTTP::Server& server) {
    server.startAsync();
    for (int i = 0; i < 50 && server.boundPort() == 0; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    const uint16_t port = server.boundPort();
    assert(port != 0);
    return port;
}

} // namespace

int main() {
    fs::path tmp = fs::temp_directory_path() / "hivemind_test_secret_routes";
    fs::remove_all(tmp);
    fs::create_directories(tmp);

    AgentStore agents{tmp / "agents.db"};
    hivemind::SecretStore secrets{tmp / "secrets.db", tmp / "secrets.key"};
    const auto agent = agents.create_agent("Grantee", "assistant", "");
    const std::string aid = agent.id;
    const auto other = agents.create_agent("Other", "assistant", "");
    const std::string oid = other.id;

    assert(secrets.put("TOKEN_ONE", "value-one"));
    assert(secrets.put("TOKEN_TWO", "value-two"));

    std::vector<std::string> notified;
    OmniNet::HTTP::Server::Config cfg;  cfg.port = 0;
    OmniNet::HTTP::Server server{cfg};
    register_secret_routes(server, agents, secrets,
        [&notified](const std::string& agent_id) {
            notified.push_back(agent_id);
        });
    const uint16_t port = start(server);

    // ── grant toggle fires the callback with the agent id ───────────────
    auto r = request("POST", port, "/api/agents/" + aid + "/secrets",
                     json{{"name", "TOKEN_ONE"}, {"granted", true}});
    assert(r.statusCode() == 200);
    assert(notified.size() == 1 && notified.back() == aid);

    // Re-grant (idempotent store op) still notifies — the push is the
    // propagation mechanism, so it must fire on every toggle call.
    r = request("POST", port, "/api/agents/" + aid + "/secrets",
                json{{"name", "TOKEN_ONE"}, {"granted", true}});
    assert(r.statusCode() == 200);
    assert(notified.size() == 2 && notified.back() == aid);

    // Revoke fires too — the env must SHRINK on the respawn.
    r = request("POST", port, "/api/agents/" + aid + "/secrets",
                json{{"name", "TOKEN_ONE"}, {"granted", false}});
    assert(r.statusCode() == 200);
    assert(notified.size() == 3 && notified.back() == aid);

    // Unknown secret / unknown agent: 404 and NO callback.
    r = request("POST", port, "/api/agents/" + aid + "/secrets",
                json{{"name", "NOPE"}, {"granted", true}});
    assert(r.statusCode() == 404);
    r = request("POST", port,
                "/api/agents/00000000-0000-0000-0000-000000000000/secrets",
                json{{"name", "TOKEN_ONE"}, {"granted", true}});
    assert(r.statusCode() == 404);
    assert(notified.size() == 3);

    // ── secret delete cascades a notification to every grantee ──────────
    assert(secrets.grant("TOKEN_TWO", aid));
    assert(secrets.grant("TOKEN_TWO", oid));
    notified.clear();
    r = request("DELETE", port, "/api/secrets/TOKEN_TWO");
    assert(r.statusCode() == 200);
    assert(notified.size() == 2);
    assert(std::find(notified.begin(), notified.end(), aid) != notified.end());
    assert(std::find(notified.begin(), notified.end(), oid) != notified.end());

    // Deleting a secret with no grants fires nothing.
    assert(secrets.put("TOKEN_LONELY", "x"));
    notified.clear();
    r = request("DELETE", port, "/api/secrets/TOKEN_LONELY");
    assert(r.statusCode() == 200);
    assert(notified.empty());

    // Deleting a missing secret: 404, no callback.
    r = request("DELETE", port, "/api/secrets/TOKEN_LONELY");
    assert(r.statusCode() == 404);
    assert(notified.empty());

    // ── a throwing callback must never fail the route ────────────────────
    {
        OmniNet::HTTP::Server::Config cfg2;  cfg2.port = 0;
        OmniNet::HTTP::Server server2{cfg2};
        register_secret_routes(server2, agents, secrets,
            [](const std::string&) {
                throw std::runtime_error("worker offline mid-toggle");
            });
        const uint16_t port2 = start(server2);
        r = request("POST", port2, "/api/agents/" + aid + "/secrets",
                    json{{"name", "TOKEN_ONE"}, {"granted", true}});
        assert(r.statusCode() == 200);
    }

    std::cout << "test_secret_routes: all assertions passed\n";
    return 0;
}
