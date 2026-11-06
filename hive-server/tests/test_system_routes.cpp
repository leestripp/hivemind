#include "hivemind/routes/SystemRoutes.h"
#include "hivemind/AgentStore.h"
#include "hivemind/HostStats.h"
#include "hivemind/SystemMonitor.h"
#include "hivemind/ThreadStore.h"
#include "hivemind/WorkerChannel.h"
#include "hivemind/WorkerStore.h"
#include "hivemind/WorkerTelemetry.h"

#include <OmniNet.h>
#include <nlohmann/json.hpp>

#include <cassert>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <thread>
#include <unistd.h>

using nlohmann::json;

namespace {

json get(uint16_t port, const std::string& path) {
    OmniNet::HTTP::ClientRequest req;
    req.method("GET").url("http://127.0.0.1:" + std::to_string(port) + path);
    auto response = OmniNet::HTTP::Client::get_instance().send_blocking(req);
    assert(response && "system route request failed");
    assert(response->statusCode() == 200);
    return json::parse(response->bodyAsString());
}

}  // namespace

int main() {
    const auto tmp = std::filesystem::temp_directory_path() /
        ("hivemind-test-system-routes-" + std::to_string(getpid()));
    std::filesystem::remove_all(tmp);
    std::filesystem::create_directories(tmp);
    setenv("HOME", tmp.c_str(), 1);  // keep exec-audit access isolated too

    AgentStore agents{tmp / "agents.db"};
    ThreadStore threads{tmp / "threads.db"};
    WorkerStore workers{tmp / "workers.db"};
    WorkerChannel channel{workers, 0, "127.0.0.1"};  // offline is expected
    WorkerTelemetry telemetry;
    SystemMonitor monitor;  // no background thread needed for this API test
    workers.enroll("rig-a");
    workers.enroll("rig-b");

    hivemind::HostStatsSampler sampler;
    auto host = hivemind::host_facts_json();
    host["hostname"] = "worker-host-a";
    auto now = hivemind::host_now_json(sampler.sample());
    now["cpu_pct"] = 42.5;
    assert(telemetry.record("rig-a", host, now));

    OmniNet::HTTP::Server::Config config;
    config.port = 0;
    OmniNet::HTTP::Server server{config};
    register_system_routes(server, monitor, telemetry, workers, channel,
                           agents, threads);
    server.startAsync();
    for (int i = 0; i < 50 && server.boundPort() == 0; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    const uint16_t port = server.boundPort();
    assert(port != 0);

    const auto snapshot = get(port, "/api/system");
    // Existing hub-only clients keep their original fields.
    assert(snapshot.contains("host") && snapshot.contains("now"));
    assert(snapshot["now"].contains("server_rss_mb"));
    assert(snapshot["workers"].size() == 2);
    const auto& a = snapshot["workers"][0];
    const auto& b = snapshot["workers"][1];
    assert(a["name"] == "rig-a" && a["telemetry"] == true);
    assert(a["online"] == false);
    assert(a["host"]["hostname"] == "worker-host-a");
    assert(a["now"]["cpu_pct"] == 42.5);
    assert(a["now"]["worker_rss_mb"] == a["now"]["rss_mb"]);
    assert(b["name"] == "rig-b" && b["telemetry"] == false);
    assert(!b.contains("now"));

    const auto worker_history = get(port, "/api/system/history?minutes=60&worker=rig-a");
    assert(worker_history["worker"] == "rig-a");
    assert(worker_history["minutes"] == 60);
    assert(worker_history["samples"].size() == 1);
    assert(worker_history["samples"][0]["cpu"] == 42.5);
    const auto empty_history = get(port, "/api/system/history?worker=rig-b");
    assert(empty_history["worker"] == "rig-b");
    assert(empty_history["samples"].empty());
    const auto hub_history = get(port, "/api/system/history?minutes=60");
    assert(!hub_history.contains("worker"));

    const auto usage = get(port, "/api/system/agents");
    assert(usage["agents"].empty());
    assert(usage["user_messages_24h"] == 0);

    server.stop();
    std::filesystem::remove_all(tmp);
    std::cout << "test_system_routes: OK\n";
    return 0;
}
