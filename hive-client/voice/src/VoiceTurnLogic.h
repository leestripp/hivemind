#pragma once

namespace hv::voice_turn {

// Thread activity and agent activity answer different questions during
// agent-to-agent relay chains. The selected agent can be idle in this DM
// while still working in a pair thread; only the agent-wide signal says
// whether the voice session should remain active.
struct ActivityScope {
    bool thread_busy;
    bool agent_busy;
};

[[nodiscard]] constexpr bool inFlight(ActivityScope activity) noexcept {
    return activity.agent_busy;
}

[[nodiscard]] constexpr bool workingOffThread(
        ActivityScope activity) noexcept {
    return activity.agent_busy && !activity.thread_busy;
}

}  // namespace hv::voice_turn

namespace hv::voice_turn {

// Post-turn audio drain may wait for synthesis (which has its own HTTP
// timeout) or playback, but never forever. `speaking` remains true through
// queued playback, so only a silent, orphaned completion can expire.
inline bool audioDrainExpired(bool speaking, long silentMs, long limitMs) {
    return !speaking && silentMs >= limitMs;
}

}  // namespace hv::voice_turn
