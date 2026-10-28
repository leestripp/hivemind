// Regression coverage for the 2026-10-28 silent ubuntu-pc outage:
// only a NEW hub acknowledgement extends the liveness deadline.

#include "heartbeat_watchdog.h"

#include <cassert>
#include <chrono>
#include <iostream>

using hwliveness::HeartbeatWatchdog;
using namespace std::chrono_literals;

int main() {
    const auto connected = HeartbeatWatchdog::Clock::time_point{};
    HeartbeatWatchdog watchdog{connected, 60s};

    assert(!watchdog.expired(connected));
    assert(!watchdog.expired(connected + 59s));
    assert(watchdog.expired(connected + 60s));

    // Polling the same count is not proof of progress.
    watchdog.acknowledge(0, connected + 30s);
    assert(watchdog.expired(connected + 60s));

    watchdog.acknowledge(1, connected + 60s);
    assert(!watchdog.expired(connected + 60s));
    assert(!watchdog.expired(connected + 119s));
    assert(watchdog.expired(connected + 120s));

    watchdog.acknowledge(1, connected + 90s);
    assert(watchdog.expired(connected + 120s));

    watchdog.acknowledge(2, connected + 110s);
    assert(!watchdog.expired(connected + 169s));
    assert(watchdog.expired(connected + 170s));

    std::cout << "test_heartbeat_watchdog: OK\n";
    return 0;
}
