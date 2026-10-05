#include "AgentStateAnnouncer.h"

#include <utility>

AgentStateAnnouncer::AgentStateAnnouncer(std::chrono::milliseconds dwell)
    : dwell_(dwell) {}

std::string friendlyState(bool online, bool busy) {
    if (!online) return "offline";
    if (busy) return "busy";
    return "ready";
}

std::vector<Announcement> AgentStateAnnouncer::diff(
    const std::vector<AgentSnapshot>& now) {
    using clock = std::chrono::steady_clock;
    const auto t = clock::now();
    std::vector<Announcement> out;

    for (const auto& a : now) {
        Seen* prev = nullptr;
        for (auto& s : seen_)
            if (s.id == a.id) { prev = &s; break; }
        if (!seeded_ || prev == nullptr) {
            seen_.push_back({a.id, a.name, a.online, a.busy, t});
            if (!seeded_ && (a.busy || !a.online))
                out.push_back(
                    {a.id, a.name + " is " + friendlyState(a.online, a.busy)});
            continue;
        }
        std::string prevName = prev->name;
        std::string prevState = friendlyState(prev->online, prev->busy);
        std::string newState = friendlyState(a.online, a.busy);
        if (prevState != newState && t - prev->since >= dwell_) {
            out.push_back({a.id, prevName + " is " + newState});
            prev->since = t;
        }
        prev->name = a.name;
        prev->online = a.online;
        prev->busy = a.busy;
    }
    seeded_ = true;
    return out;
}
