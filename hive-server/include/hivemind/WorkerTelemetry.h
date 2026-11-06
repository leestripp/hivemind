#pragma once

#include "hivemind/HostStats.h"

#include <deque>
#include <mutex>
#include <nlohmann/json.hpp>
#include <string>
#include <unordered_map>

/** Thread-safe, in-memory host telemetry received on worker heartbeats.
 * Each enrolled worker gets an independent current sample and 24 h series;
 * no SQLite and no additional network polling. */
class WorkerTelemetry {
public:
    bool record(const std::string& worker,
                const nlohmann::json& host,
                const nlohmann::json& now) noexcept;

    // Null when this worker has not sent telemetry (older worker, or before
    // its first 15-second heartbeat).
    nlohmann::json snapshot(const std::string& worker) const;
    nlohmann::json history(const std::string& worker, int minutes) const;

    static constexpr int kIntervalSec = 15;
    static constexpr int kRetainSec = 24 * 3600;

private:
    struct Series {
        hivemind::HostFacts host;
        std::deque<hivemind::HostStatsSample> samples;
    };

    mutable std::mutex mu_;
    std::unordered_map<std::string, Series> workers_;
};
