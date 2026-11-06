#include "hivemind/HostStats.h"

#include <cassert>
#include <cmath>
#include <iostream>

int main() {
    hivemind::HostStatsSampler sampler;
    const auto first = sampler.sample();
    assert(first.t > 0);
    assert(first.mem_total_kb > 0);
    assert(first.mem_avail_kb >= 0);
    assert(first.mem_avail_kb <= first.mem_total_kb);
    assert(first.disk_total_kb > 0);
    assert(first.disk_avail_kb >= 0);
    assert(first.disk_avail_kb <= first.disk_total_kb);
    assert(first.cpu_pct >= 0 && first.cpu_pct <= 100);
    assert(first.disk_pct >= 0 && first.disk_pct <= 100);
    assert(std::isfinite(first.load1) && first.load1 >= 0);

    const auto second = sampler.sample();
    assert(second.cpu_pct >= 0 && second.cpu_pct <= 100);

    const auto now = hivemind::host_now_json(second);
    const auto parsed = hivemind::host_sample_from_json(now);
    assert(parsed);
    assert(parsed->t == second.t);
    assert(parsed->mem_total_kb / 1024 == second.mem_total_kb / 1024);
    assert(parsed->disk_total_kb / 1048576 == second.disk_total_kb / 1048576);
    assert(parsed->process_rss_kb / 1024 == second.process_rss_kb / 1024);

    auto invalid = now;
    invalid["cpu_pct"] = 101;
    assert(!hivemind::host_sample_from_json(invalid));
    invalid = now;
    invalid.erase("rss_mb");
    assert(!hivemind::host_sample_from_json(invalid));
    invalid = now;
    invalid["mem_used_mb"] = invalid["mem_total_mb"].get<int64_t>() + 1;
    assert(!hivemind::host_sample_from_json(invalid));

    const auto facts_json = hivemind::host_facts_json();
    const auto facts = hivemind::host_facts_from_json(facts_json);
    assert(facts);
    assert(!facts->hostname.empty());
    assert(facts->cores > 0);
    assert(facts->uptime_s >= 0);
    auto bad_facts = facts_json;
    bad_facts["hostname"] = "";
    assert(!hivemind::host_facts_from_json(bad_facts));

    std::cout << "test_host_stats: OK\n";
    return 0;
}
