#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <nlohmann/json.hpp>

namespace hivemind {

struct HostStatsSample {
    int64_t t = 0;              // epoch seconds (UTC)
    double cpu_pct = 0;         // 0–100, all cores (delta of /proc/stat)
    double load1 = 0;
    int64_t mem_total_kb = 0;
    int64_t mem_avail_kb = 0;
    double disk_pct = 0;        // usage of the / filesystem
    int64_t disk_total_kb = 0;
    int64_t disk_avail_kb = 0;
    int64_t process_rss_kb = 0;
    int containers = 0;         // running hive-* podman containers
};

struct HostFacts {
    std::string hostname;
    std::string kernel;
    int cores = 0;
    int64_t uptime_s = 0;
};

/** Linux host sampler shared by the hub and every worker. One sampler per
 * process: CPU utilization is calculated between calls, not since boot. */
class HostStatsSampler {
public:
    HostStatsSample sample();

private:
    int count_containers();

    uint64_t prev_total_ = 0;
    uint64_t prev_idle_ = 0;
    bool podman_checked_ = false;
    bool podman_ok_ = false;
};

HostFacts host_facts();
nlohmann::json host_facts_json(const HostFacts& facts);
nlohmann::json host_facts_json();
nlohmann::json host_now_json(const HostStatsSample& sample);
nlohmann::json host_history_json(const HostStatsSample& sample);

// Worker heartbeat input is untrusted until normalized. Invalid or partial
// telemetry is rejected without disturbing the liveness/agent-state channel.
std::optional<HostFacts> host_facts_from_json(const nlohmann::json& value);
std::optional<HostStatsSample> host_sample_from_json(
    const nlohmann::json& value);

}  // namespace hivemind
