#include "hivemind/WorkerTelemetry.h"

#include <cassert>
#include <ctime>
#include <iostream>

using nlohmann::json;

namespace {

json host_for(const char* name) {
    auto host = hivemind::host_facts_json();
    host["hostname"] = name;
    return host;
}

json sample_for(hivemind::HostStatsSample base, int64_t t, double cpu) {
    base.t = t;
    base.cpu_pct = cpu;
    return hivemind::host_now_json(base);
}

}  // namespace

int main() {
    WorkerTelemetry telemetry;
    hivemind::HostStatsSampler sampler;
    const auto base = sampler.sample();
    const int64_t now = std::time(nullptr);

    assert(telemetry.snapshot("rig-a").is_null());
    assert(telemetry.record("rig-a", host_for("rig-a"),
                            sample_for(base, now - 25 * 3600, 5)));
    assert(telemetry.record("rig-a", host_for("rig-a"),
                            sample_for(base, now - 30, 10)));
    assert(telemetry.record("rig-b", host_for("rig-b"),
                            sample_for(base, now - 20, 20)));
    assert(telemetry.record("rig-a", host_for("rig-a"),
                            sample_for(base, now - 10, 30)));

    const auto a = telemetry.snapshot("rig-a");
    const auto b = telemetry.snapshot("rig-b");
    assert(a["host"]["hostname"] == "rig-a");
    assert(b["host"]["hostname"] == "rig-b");
    assert(a["now"]["cpu_pct"] == 30);
    assert(b["now"]["cpu_pct"] == 20);
    assert(a["now"]["worker_rss_mb"] == a["now"]["rss_mb"]);
    assert(a["history_samples"] == 2);  // 25-hour-old sample was pruned
    assert(b["history_samples"] == 1);

    const auto a_hist = telemetry.history("rig-a", 60);
    const auto b_hist = telemetry.history("rig-b", 60);
    assert(a_hist["worker"] == "rig-a");
    assert(a_hist["samples"].size() == 2);
    assert(b_hist["samples"].size() == 1);
    assert(a_hist["samples"][0]["cpu"] == 10);
    assert(a_hist["samples"][1]["cpu"] == 30);
    assert(telemetry.history("missing", 60)["samples"].empty());
    assert(telemetry.history("rig-a", 99999)["minutes"] == 1440);

    // Same timestamp replaces (reconnect/retry), older data cannot rewind.
    assert(telemetry.record("rig-a", host_for("rig-a"),
                            sample_for(base, now - 10, 35)));
    assert(telemetry.snapshot("rig-a")["history_samples"] == 2);
    assert(telemetry.snapshot("rig-a")["now"]["cpu_pct"] == 35);
    assert(!telemetry.record("rig-a", host_for("rig-a"),
                             sample_for(base, now - 20, 99)));
    assert(telemetry.snapshot("rig-a")["now"]["cpu_pct"] == 35);

    // Malformed ancillary telemetry is ignored, not allowed to poison a beat.
    auto invalid = sample_for(base, now, 40);
    invalid["disk_pct"] = 120;
    assert(!telemetry.record("rig-a", host_for("rig-a"), invalid));
    assert(!telemetry.record("rig-a", json::object(), sample_for(base, now, 40)));

    std::cout << "test_worker_telemetry: OK\n";
    return 0;
}
