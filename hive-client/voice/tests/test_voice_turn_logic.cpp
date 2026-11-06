#include "VoiceTurnLogic.h"

#include <catch2/catch_test_macros.hpp>

using hv::voice_turn::ActivityScope;
using hv::voice_turn::SharedAgentState;
using hv::voice_turn::effectiveAgentBusy;
using hv::voice_turn::inFlight;
using hv::voice_turn::readyCompletionPollNeeded;
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


TEST_CASE("audio drain only expires while silent past its bound") {
    using hv::voice_turn::audioDrainExpired;
    REQUIRE_FALSE(audioDrainExpired(true, 90000, 75000));
    REQUIRE_FALSE(audioDrainExpired(false, 74999, 75000));
    REQUIRE(audioDrainExpired(false, 75000, 75000));
}

TEST_CASE("shared roster ready wins over stale DM activity", "[voice-turn]") {
    REQUIRE_FALSE(effectiveAgentBusy(true, {.known = true, .busy = false}));
    REQUIRE_FALSE(effectiveAgentBusy(false, {.known = true, .busy = false}));
}

TEST_CASE("shared roster busy covers work in another thread", "[voice-turn]") {
    REQUIRE(effectiveAgentBusy(false, {.known = true, .busy = true}));
    REQUIRE(effectiveAgentBusy(true, {.known = true, .busy = true}));
}

TEST_CASE("first roster snapshot missing uses DM activity", "[voice-turn]") {
    REQUIRE(effectiveAgentBusy(true, {.known = false, .busy = false}));
    REQUIRE_FALSE(effectiveAgentBusy(false, {.known = false, .busy = false}));
}

TEST_CASE("ready never cancels transcription or send", "[voice-turn]") {
    const SharedAgentState ready{.known = true, .busy = false};
    REQUIRE_FALSE(readyCompletionPollNeeded(ready, false, false));
}

TEST_CASE("ready repairs only an unfinished streaming narration", "[voice-turn]") {
    const SharedAgentState ready{.known = true, .busy = false};
    REQUIRE(readyCompletionPollNeeded(ready, false, true));
    REQUIRE_FALSE(readyCompletionPollNeeded(ready, true, true));
    REQUIRE_FALSE(readyCompletionPollNeeded(
            {.known = true, .busy = true}, false, true));
    REQUIRE_FALSE(readyCompletionPollNeeded(
            {.known = false, .busy = false}, false, true));
}
