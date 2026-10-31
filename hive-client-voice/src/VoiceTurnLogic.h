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
