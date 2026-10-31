#include "VoiceTurnLogic.h"

#include <catch2/catch_test_macros.hpp>

using hv::voice_turn::ActivityScope;
using hv::voice_turn::inFlight;
using hv::voice_turn::workingOffThread;

TEST_CASE("agent-wide idle completes a voice turn", "[voice-turn]") {
    REQUIRE_FALSE(inFlight({.thread_busy = false, .agent_busy = false}));
    REQUIRE_FALSE(workingOffThread(
            {.thread_busy = false, .agent_busy = false}));
}

TEST_CASE("ordinary DM turn remains active", "[voice-turn]") {
    const ActivityScope activity{.thread_busy = true, .agent_busy = true};
    REQUIRE(inFlight(activity));
    REQUIRE_FALSE(workingOffThread(activity));
}

TEST_CASE("agent-to-agent work survives an idle DM", "[voice-turn]") {
    const ActivityScope activity{.thread_busy = false, .agent_busy = true};
    REQUIRE(inFlight(activity));
    REQUIRE(workingOffThread(activity));
}

TEST_CASE("agent-wide idle wins over a stale thread snapshot",
          "[voice-turn]") {
    const ActivityScope activity{.thread_busy = true, .agent_busy = false};
    REQUIRE_FALSE(inFlight(activity));
    REQUIRE_FALSE(workingOffThread(activity));
}
