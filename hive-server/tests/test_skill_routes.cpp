// HTTP boundary for curated starters: no arbitrary installs, per-agent
// toggles, or workspace/private-skill inventory survives the simplified API.
#include "hivemind/routes/SkillRoutes.h"
#include "hivemind/AgentStore.h"
#include "hivemind/WorkerChannel.h"
#include "hivemind/WorkerStore.h"

#include <OmniNet.h>
#include <nlohmann/json.hpp>

#undef NDEBUG
#include <cassert>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <thread>

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {
OmniNet::HTTP::ClientResponse request(const std::string& method, uint16_t port,
                                      const std::string& path,
                                      const json& body = json()) {
    OmniNet::HTTP::ClientRequest req;
    req.method(method)
       .url("http://127.0.0.1:" + std::to_string(port) + path)
       .header("Content-Type", "application/json");
    if (!body.is_null()) req.body(body.dump());
    auto response = OmniNet::HTTP::Client::get_instance().send_blocking(req);
    assert(response);
    return *response;
}
}

int main() {
    const fs::path tmp = fs::temp_directory_path() / "hivemind_test_skill_routes";
    fs::remove_all(tmp);
    fs::create_directories(tmp / "web/dist");
    setenv("HOME", tmp.c_str(), 1);
    setenv("PWD", tmp.c_str(), 1);
    {
        std::ofstream(tmp / "web/dist/skills-registry.json") << R"json(
            {"entries":[
              {"name":"sample","description":"Curated sample",
               "git_url":"https://example.invalid/sample.git"},
              {"name":"other","description":"Not installed",
               "git_url":"https://example.invalid/other.git", "path":"/absolute"}
            ]})json";
    }
    const fs::path cache = tmp / ".local/share/hivemind/skills";
    fs::create_directories(cache / "sample");
    { std::ofstream(cache / "sample/SKILL.md")
          << "---\nname: sample\ndescription: Curated sample\n---\nbody\n"; }
    // A same-cache non-catalog directory is not an installed starter and is
    // never exposed as a catalog entry.
    fs::create_directories(cache / "agent-owned");
    { std::ofstream(cache / "agent-owned/SKILL.md") << "private content\n"; }
    const fs::path personal = tmp / "personal/sample";
    fs::create_directories(personal);
    { std::ofstream(personal / "SKILL.md") << "agent private copy\n"; }

    AgentStore agents{tmp / "agents.db"};
    WorkerStore workers{tmp / "workers.db"};
    WorkerChannel channel{workers, 0, "127.0.0.1"};
    OmniNet::HTTP::Server::Config config;
    config.port = 0;
    OmniNet::HTTP::Server server{config};
    register_skill_routes(server, workers, channel);
    server.startAsync();
    for (int i = 0; i < 50 && server.boundPort() == 0; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    const uint16_t port = server.boundPort();
    assert(port != 0);

    auto response = request("GET", port, "/api/skills");
    assert(response.statusCode() == 200);
    const auto listed = json::parse(response.bodyAsString())["skills"];
    assert(listed.size() == 1 && listed[0]["name"] == "sample");

    response = request("GET", port, "/api/skills/registry");
    auto entries = json::parse(response.bodyAsString())["entries"];
    assert(entries.size() == 2);
    assert(entries[0]["installed"] == true && entries[1]["installed"] == false);

    // Name-only curated installation; old arbitrary-Git and per-agent routes
    // are intentionally gone.
    response = request("POST", port, "/api/skills/install",
                       {{"git_url", "https://example.invalid/anything.git"}});
    assert(response.statusCode() == 400);
    response = request("POST", port, "/api/skills/install",
                       {{"name", "not-in-the-catalog"}});
    assert(response.statusCode() == 400);
    response = request("POST", port, "/api/skills/install", {{"name", "other"}});
    assert(response.statusCode() == 500);  // unsafe curated path rejected pre-clone
    response = request("POST", port, "/api/skills/install", {{"name", "sample"}});
    assert(response.statusCode() == 409);
    assert(request("POST", port, "/api/skills", json::object()).statusCode() == 404);
    assert(request("POST", port, "/api/agents/a/global-skills",
                   {{"name", "sample"}, {"enabled", true}}).statusCode() == 404);
    assert(request("GET", port, "/api/skills/workspace").statusCode() == 404);

    // Uninstall refuses to strand offline workers with undeletable copies.
    workers.enroll("offline-rig");
    response = request("DELETE", port, "/api/skills/sample");
    assert(response.statusCode() == 409);
    assert(json::parse(response.bodyAsString())["offline_workers"][0] == "offline-rig");
    assert(fs::exists(cache / "sample"));

    // With no agent workers to update, removal touches only the curated cache.
    assert(workers.remove("offline-rig"));
    response = request("DELETE", port, "/api/skills/sample");
    assert(response.statusCode() == 200);
    assert(!fs::exists(cache / "sample"));
    assert(fs::exists(cache / "agent-owned"));
    assert(fs::exists(personal / "SKILL.md"));
    assert(request("DELETE", port, "/api/skills/sample").statusCode() == 404);

    server.stop();
    fs::remove_all(tmp);
    std::cout << "test_skill_routes: all passed\n";
}
