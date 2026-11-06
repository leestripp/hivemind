#include "hivemind/WorkerTelemetry.h"

#include <ctime>

using nlohmann::json;

bool WorkerTelemetry::record(const std::string& worker, const json& host,
                             const json& now) noexcept {
    try {
        const auto facts = hivemind::host_facts_from_json(host);
        const auto sample = hivemind::host_sample_from_json(now);
        if (worker.empty() || !facts || !sample) return false;

        std::lock_guard lk(mu_);
        auto& series = workers_[worker];
        if (!series.samples.empty()) {
            const auto last_t = series.samples.back().t;
            if (sample->t < last_t) return false;
            if (sample->t == last_t) series.samples.pop_back();
        }
        series.host = *facts;
        series.samples.push_back(*sample);
        const int64_t cutoff = sample->t - kRetainSec;
        while (!series.samples.empty() && series.samples.front().t < cutoff)
            series.samples.pop_front();
        return true;
    } catch (...) {
        // Telemetry is ancillary. A malformed frame must not affect worker
        // liveness, agent state, or the heartbeat_ack path.
        return false;
    }
}

json WorkerTelemetry::snapshot(const std::string& worker) const {
    std::lock_guard lk(mu_);
    const auto it = workers_.find(worker);
    if (it == workers_.end() || it->second.samples.empty()) return nullptr;
    const auto& series = it->second;
    auto now = hivemind::host_now_json(series.samples.back());
    now["worker_rss_mb"] = series.samples.back().process_rss_kb / 1024;
    return json{{"host", hivemind::host_facts_json(series.host)},
                {"now", std::move(now)},
                {"history_samples", series.samples.size()},
                {"interval_s", kIntervalSec}};
}

json WorkerTelemetry::history(const std::string& worker, int minutes) const {
    if (minutes <= 0) minutes = 60;
    if (minutes > kRetainSec / 60) minutes = kRetainSec / 60;
    const int64_t cutoff = static_cast<int64_t>(std::time(nullptr)) -
                           static_cast<int64_t>(minutes) * 60;
    json samples = json::array();
    {
        std::lock_guard lk(mu_);
        const auto it = workers_.find(worker);
        if (it != workers_.end()) {
            for (const auto& sample : it->second.samples)
                if (sample.t >= cutoff)
                    samples.push_back(hivemind::host_history_json(sample));
        }
    }
    return json{{"worker", worker}, {"minutes", minutes},
                {"samples", std::move(samples)}};
}
