// hive-worker — heartbeat acknowledgement watchdog.
// See docs/hive-worker.md "Channel protocol".
#pragma once

#include <chrono>
#include <cstdint>

namespace hwliveness {

/** Tracks hub heartbeat acknowledgements for one worker connection. */
class HeartbeatWatchdog {
public:
    using Clock = std::chrono::steady_clock;

    HeartbeatWatchdog(Clock::time_point connected_at,
                      std::chrono::seconds timeout);

    /// Records the latest cumulative acknowledgement count. Repeated counts
    /// do not extend the connection's liveness deadline.
    void acknowledge(uint64_t count, Clock::time_point now);

    [[nodiscard]] bool expired(Clock::time_point now) const;

private:
    uint64_t last_count_;
    Clock::time_point last_progress_;
    std::chrono::seconds timeout_;
};

}  // namespace hwliveness
