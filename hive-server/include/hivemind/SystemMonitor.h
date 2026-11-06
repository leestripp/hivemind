#pragma once

#include "hivemind/HostStats.h"

#include <atomic>
#include <deque>
#include <mutex>
#include <thread>
#include <nlohmann/json.hpp>

/**
 * SystemMonitor — lightweight host metrics sampler for the web UI System
 * page. A single thread reads /proc every kIntervalSec and appends to an
 * in-memory ring buffer (kRetainSec of history). The worker heartbeat uses
 * the same HostStatsSampler, so remote and hub measurements are comparable.
 *
 * The sampler always runs; the web page only POLLS while it is active.
 * Nothing survives a restart (fine for a dashboard).
 */
class SystemMonitor {
public:
    using Sample = hivemind::HostStatsSample;

    SystemMonitor() = default;
    ~SystemMonitor() { stop(); }

    void start();
    void stop();

    /// Current sample + static host facts (hostname, cores, boot time).
    nlohmann::json snapshot();
    /// Samples from the last `minutes`, oldest first (capped at retention).
    nlohmann::json history(int minutes);

    static constexpr int kIntervalSec = 15;
    static constexpr int kRetainSec = 24 * 3600;

private:
    std::thread thread_;
    std::atomic<bool> running_{false};
    mutable std::mutex mu_;
    std::deque<Sample> samples_;
    hivemind::HostStatsSampler sampler_;
};
