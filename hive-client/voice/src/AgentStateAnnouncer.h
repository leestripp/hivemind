// AgentStateAnnouncer — edge-triggered, dwell-filtered agent-state
// announcements ("Scout is now online"). 1:1 port of the Kotlin
// AgentStateAnnouncer. Pure logic, unit-tested.
#pragma once

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

struct AgentSnapshot {
    std::string id;
    std::string name;
    bool online{false};
    bool busy{false};
};

struct Announcement {
    std::string agentId;
    std::string text;
};

class AgentStateAnnouncer {
public:
    explicit AgentStateAnnouncer(std::chrono::milliseconds dwell =
                                     std::chrono::milliseconds(15000));

    // Diff a fresh roster against the dwelled previous state.
    std::vector<Announcement> diff(const std::vector<AgentSnapshot>& now);

    // Force the next diff to treat everything as new.
    void reset() { seeded_ = false; }

private:
    struct Seen {
        std::string id;
        std::string name;
        bool online{false};
        bool busy{false};
        std::chrono::steady_clock::time_point since;
    };
    std::chrono::milliseconds dwell_;
    bool seeded_{false};
    std::vector<Seen> seen_;
};

std::string friendlyState(bool online, bool busy);
