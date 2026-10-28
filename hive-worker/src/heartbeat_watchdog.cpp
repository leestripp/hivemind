// hive-worker — heartbeat acknowledgement watchdog.
// See docs/hive-worker.md "Channel protocol".

#include "heartbeat_watchdog.h"

namespace hwliveness {

HeartbeatWatchdog::HeartbeatWatchdog(Clock::time_point connected_at,
                                     std::chrono::seconds timeout)
    : last_count_(0), last_progress_(connected_at), timeout_(timeout) {}

void HeartbeatWatchdog::acknowledge(uint64_t count, Clock::time_point now) {
    if (count == last_count_) return;
    last_count_ = count;
    last_progress_ = now;
}

bool HeartbeatWatchdog::expired(Clock::time_point now) const {
    return now - last_progress_ >= timeout_;
}

}  // namespace hwliveness
