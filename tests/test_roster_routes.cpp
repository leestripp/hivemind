// tests/test_roster_routes.cpp — Phase 11.6: HTTP-level tests for the
// queue-driven chat routes (fan-in, cancel, delete) + error paths.
// Spins a real OmniNet server on an ephemeral port; no LLM (turns fail
// fast in the background — queue semantics are what's under test).
#include "hivemind/routes/RosterRoutes.h"
#include "hivemind/LLMStore.h"
#include "hivemind/ApprovalStore.h"
#include "hivemind/RoutineStore.h"

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

} // namespace

int main() {
    fs::path tmp = fs::temp_directory_path() / "hivemind_test_roster_routes";
    fs::remove_all(tmp);
    fs::create_directories(tmp);

    AgentStore agents{tmp / "agents.db"};
    ThreadStore threads{tmp / "threads.db"};
    ApprovalStore approvals{tmp / "approvals.db"};
    LLMStore llm{tmp / "llm.db"};
    RoutineStore routines{tmp / "routines.db"};
    AgentRegistry registry{agents, approvals, llm};
    ChatService chat{agents, threads, registry};

    OmniNet::HTTP::Server::Config cfg;
    cfg.port = 0;                       // ephemeral
    OmniNet::HTTP::Server server{cfg};
    register_roster_routes(server, agents, threads, registry, chat, routines);
    server.startAsync();
    // wait for bind (port() is the CONFIGURED port; boundPort() is actual)
    for (int i = 0; i < 50 && server.boundPort() == 0; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    const uint16_t port = server.boundPort();
    assert(port != 0);

    // ── setup: two roster agents ──────────────────────────────────────────
    auto r = request("POST", port, "/api/agents",
                     {{"name", "Alpha"}, {"role", "coder"}});
    assert(r.statusCode() == 201);
    const std::string a1 = json::parse(r.bodyAsString())["id"];
    r = request("POST", port, "/api/agents", {{"name", "Beta"}, {"role", "reviewer"}});
    const std::string a2 = json::parse(r.bodyAsString())["id"];

    // ── thread create validation ──────────────────────────────────────────
    r = request("POST", port, "/api/threads", {{"agent_ids", json::array()}});
    assert(r.statusCode() == 400);                       // empty agent_ids
    r = request("POST", port, "/api/threads",
                {{"agent_ids", json::array({"no-such-agent"})}});
    assert(r.statusCode() == 400);                       // unknown agent

    // ── group thread (fan-in target) ──────────────────────────────────────
    r = request("POST", port, "/api/threads",
                {{"title", "design review"}, {"kind", "group"},
                 {"agent_ids", json::array({a1, a2})}});
    assert(r.statusCode() == 201);
    const std::string gid = json::parse(r.bodyAsString())["id"];

    // ── DM dedup: reuse_dm returns the same thread ────────────────────────
    r = request("POST", port, "/api/threads",
                {{"agent_ids", json::array({a1})}, {"reuse_dm", true}});
    assert(r.statusCode() == 201);
    const std::string dm1 = json::parse(r.bodyAsString())["id"];
    r = request("POST", port, "/api/threads",
                {{"agent_ids", json::array({a1})}, {"reuse_dm", true}});
    assert(r.statusCode() == 200 && json::parse(r.bodyAsString())["id"] == dm1);
    // without reuse_dm a fresh thread is created
    r = request("POST", port, "/api/threads", {{"agent_ids", json::array({a1})}});
    assert(json::parse(r.bodyAsString())["id"] != dm1);

    // ── queue: post a message → 202 processing, message persisted ─────────
    r = request("POST", port, "/api/threads/" + gid + "/messages",
                {{"body", "what do you two think?"}});
    assert(r.statusCode() == 202);
    auto posted = json::parse(r.bodyAsString());
    assert(posted["status"] == "processing");
    assert(posted.contains("message_id"));

    auto msgs = jget(port, "/api/threads/" + gid + "/messages");
    assert(msgs["messages"].size() == 1);
    assert(msgs["messages"][0]["body"] == "what do you two think?");

    // empty body rejected
    r = request("POST", port, "/api/threads/" + gid + "/messages", {{"body", ""}});
    assert(r.statusCode() == 400);

    // ── activity endpoint shape (turns may already have failed — no LLM) ──
    auto act = jget(port, "/api/threads/" + gid + "/activity");
    assert(act.contains("busy") && act.contains("turns"));

    // ── cancel: 202, idempotent-ish (no live turns is fine) ───────────────
    r = request("POST", port, "/api/threads/" + gid + "/cancel", json::object());
    assert(r.statusCode() == 202);
    r = request("POST", port, "/api/threads/no-such/cancel", json::object());
    assert(r.statusCode() == 404);

    // ── fan-in: both agents were participants of the group thread ─────────
    auto tj = jget(port, "/api/threads/" + gid);
    assert(tj["agent_ids"].size() == 2);

    // ── delete: thread gone afterwards ────────────────────────────────────
    r = request("DELETE", port, "/api/threads/" + gid);
    assert(r.statusCode() == 200);
    r = request("GET", port, "/api/threads/" + gid);
    assert(r.statusCode() == 404);
    r = request("DELETE", port, "/api/threads/" + gid);
    assert(r.statusCode() == 404);                       // double delete

    // messages endpoint on deleted thread 404s too
    r = request("GET", port, "/api/threads/" + gid + "/messages");
    assert(r.statusCode() == 404);

    // ── drain background turns before exit (they fail fast: no LLM) ───────
    chat.quiesce();

    // ── Phase 8: idle eviction — fresh agent (never messaged = never busy) ─
    {
        auto r3 = request("POST", port, "/api/agents",
                          {{"name", "Evictable"}, {"role", "test"}});
        assert(r3.statusCode() == 201);
        const std::string a3 = json::parse(r3.bodyAsString())["id"];
        auto agent = agents.get_agent(a3);
        assert(agent);
        auto& host = registry.get_or_create(*agent);
        assert(!host.busy());
        registry.evict_idle(std::chrono::seconds(0));   // everything idle
        auto& host2 = registry.get_or_create(*agent);   // fresh host
        assert(&host2 != &host);
        host2.touch();
        registry.evict_idle(std::chrono::hours(24));    // nothing idle that long
        assert(&registry.get_or_create(*agent) == &host2);
    }

    registry.shutdown_all();

    fs::remove_all(tmp);
    std::cout << "test_roster_routes OK\n";
    return 0;
}
