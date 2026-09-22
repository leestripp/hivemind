// tests/test_roster_routes.cpp — Phase 11.6: HTTP-level tests for the
// queue-driven chat routes (fan-in, cancel, delete) + error paths.
// Spins a real OmniNet server on an ephemeral port; no LLM (turns fail
// fast in the background — queue semantics are what's under test).
#include "hivemind/routes/RosterRoutes.h"
#include "hivemind/ChatService.h"
#include "hivemind/HubRouter.h"
#include "hivemind/WorkerChannel.h"
#include "hivemind/WorkerStore.h"
#include "hivemind/ApprovalStore.h"
#include "hivemind/RoutineStore.h"
#include "hivemind/SecretStore.h"

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
    RoutineStore routines{tmp / "routines.db"};
    WorkerStore wstore{tmp / "workers.db"};
    WorkerChannel wchannel{wstore, 0, "127.0.0.1"};   // never start()ed
    hivemind::SecretStore secrets{tmp / "secrets.db",
                                  tmp / "secrets.key"};
    HubRouter hub{agents, threads, wchannel, approvals, routines,
                  secrets};
    ChatService chat{agents, threads, hub};

    OmniNet::HTTP::Server::Config cfg;
    cfg.port = 0;                       // ephemeral
    OmniNet::HTTP::Server server{cfg};
    register_roster_routes(server, agents, threads, hub, chat, routines, wstore);
    server.startAsync();
    // wait for bind (port() is the CONFIGURED port; boundPort() is actual)
    for (int i = 0; i < 50 && server.boundPort() == 0; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    const uint16_t port = server.boundPort();
    assert(port != 0);

    // ── setup: two roster agents ──────────────────────────────────────────
    const std::string wkey = wstore.enroll("testrig");

    // Regression (2026-08-30): unknown/absent worker must be REJECTED --
    // a "local"/ghost worker creates a zombie agent that never spawns.
    auto r = request("POST", port, "/api/agents",
                     {{"name", "Zed"}, {"role", "coder"}});
    assert(r.statusCode() == 400);
    r = request("POST", port, "/api/agents",
                {{"name", "Zed"}, {"role", "coder"}, {"worker", "local"}});
    assert(r.statusCode() == 400);

    r = request("POST", port, "/api/agents",
                {{"name", "Alpha"}, {"role", "coder"}, {"worker", "testrig"}});
    assert(r.statusCode() == 201);
    const std::string a1 = json::parse(r.bodyAsString())["id"];
    r = request("POST", port, "/api/agents",
                {{"name", "Beta"}, {"role", "reviewer"}, {"worker", "testrig"}});
    const std::string a2 = json::parse(r.bodyAsString())["id"];

    // ── Phase 35: agent type validation ───────────────────────────────────
    // type=gpu on a worker with no gpu cap -> 400
    r = request("POST", port, "/api/agents",
                {{"name", "Forge"}, {"role", "render"}, {"worker", "testrig"},
                 {"type", "gpu"}});
    assert(r.statusCode() == 400);

    // testrig advertises gpu -> accepted, and type round-trips
    wstore.validate(wkey, R"({"arch":"x86_64","gpu":"amd"})");
    r = request("POST", port, "/api/agents",
                {{"name", "Forge"}, {"role", "render"}, {"worker", "testrig"},
                 {"type", "gpu"}});
    assert(r.statusCode() == 201);
    const std::string a3 = json::parse(r.bodyAsString())["id"];
    assert(json::parse(r.bodyAsString())["type"] == "gpu");
    assert(jget(port, "/api/agents/" + a3)["type"] == "gpu");   // persisted

    // moving the gpu agent onto a gpu-less worker -> 400 (effective type)
    wstore.enroll("plainrig");
    r = request("PATCH", port, "/api/agents/" + a3,
                {{"worker", "plainrig"}});
    assert(r.statusCode() == 400);

    // PATCH type=default clears back to standard
    r = request("PATCH", port, "/api/agents/" + a3, {{"type", "default"}});
    assert(r.statusCode() == 200);
    assert(jget(port, "/api/agents/" + a3)["type"] == "");

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
    // ba6c27a: a failed dispatch (workers are offline in this test) posts a
    // visible system message per participant, synchronously, right after the
    // user message — so the thread holds 1 user + N system messages. Assert
    // on the USER message, not the total count.
    int user_msgs = 0;
    for (const auto& m : msgs["messages"])
        if (m.value("sender_kind", "") == "user") {
            ++user_msgs;
            assert(m["body"] == "what do you two think?");
        }
    assert(user_msgs == 1);

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

    // ── Phase 44: User Inbox — singleton, non-deletable, loop-safe ───────
    threads.ensure_user_thread();
    auto inbox = jget(port, "/api/threads/user");
    assert(inbox["kind"] == "user");
    assert(inbox["agent_ids"].empty());      // zero participants: no fan-in
    // idempotent
    threads.ensure_user_thread();
    auto all = jget(port, "/api/threads");
    int inbox_count = 0;
    for (const auto& t : all["threads"])
        if (t.value("kind", "") == "user") ++inbox_count;
    assert(inbox_count == 1);
    // protected from deletion (route 400 + store refuses)
    r = request("DELETE", port, "/api/threads/user");
    assert(r.statusCode() == 400);
    assert(jget(port, "/api/threads/user")["id"] == "user");

    // user_msg op (message_user tool relay) posts as the sender agent
    hub.set_chat_service(&chat);
    hub.on_worker_frame("testrig", {{"op", "user_msg"}, {"agent", a1},
                                    {"text", "status for the human"}});
    auto im = jget(port, "/api/threads/user/messages");
    assert(im["messages"].size() == 1);
    assert(im["messages"][0].value("sender_kind", "") == "agent");
    assert(im["messages"][0].value("sender_id", "") == a1);

    // agent_msg with an unresolvable target ("user") redirects to the inbox
    // instead of failing — the loop vector from the 2026-09-09 incident.
    hub.on_worker_frame("testrig", {{"op", "agent_msg"}, {"agent", a1},
                                    {"target", "user"}, {"text", "ping lee"}});
    im = jget(port, "/api/threads/user/messages");
    assert(im["messages"].size() == 2);
    // no system/dispatch noise in the inbox: both messages are agent posts
    for (const auto& m : im["messages"])
        assert(m.value("sender_kind", "") == "agent");

    // ── drain background turns before exit (they fail fast: no LLM) ───────
    chat.quiesce();


    fs::remove_all(tmp);
    std::cout << "test_roster_routes OK\n";
    return 0;
}
