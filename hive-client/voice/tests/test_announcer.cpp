#include "../src/AgentStateAnnouncer.h"

#include <catch2/catch_test_macros.hpp>

#include <thread>

using namespace std::chrono_literals;

TEST_CASE("first sighting announces busy/offline only", "[announcer]") {
    AgentStateAnnouncer a(0ms);
    auto v = a.diff({{"id1", "Scout", true, false},
                     {"id2", "Bob", true, true},
                     {"id3", "Hal", false, false}});
    REQUIRE(v.size() == 2);  // Bob busy + Hal offline; ready is silent
    REQUIRE(v[0].text == "Bob is busy");
    REQUIRE(v[1].text == "Hal is offline");
}

TEST_CASE("edge announces after dwell, not before", "[announcer]") {
    AgentStateAnnouncer a(1000ms);
    a.diff({{"id1", "Scout", true, false}});
    auto early = a.diff({{"id1", "Scout", true, true}});
    REQUIRE(early.empty());
    std::this_thread::sleep_for(1100ms);
    auto late = a.diff({{"id1", "Scout", true, true}});
    REQUIRE(late.size() == 1);
    REQUIRE(late[0].text == "Scout is busy");
}

TEST_CASE("no spurious announce when unchanged", "[announcer]") {
    AgentStateAnnouncer a(0ms);
    a.diff({{"id1", "Scout", true, false}});
    REQUIRE(a.diff({{"id1", "Scout", true, false}}).empty());
}

TEST_CASE("friendlyState", "[announcer]") {
    REQUIRE(friendlyState(false, false) == "offline");
    REQUIRE(friendlyState(true, true) == "busy");
    REQUIRE(friendlyState(true, false) == "ready");
}
