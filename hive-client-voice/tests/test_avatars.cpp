// Avatars: the name→creature mapping must match hive-android Avatars.kt
// and hive-client auto_avatar exactly — all three hash the agent name
// with MurmurHash64A (libstdc++ std::hash) into the same ordered set.
// These expected values were cross-checked against a reference
// MurmurHash64A(seed 0xc70f6907) implementation.
#include "../src/Avatars.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>

TEST_CASE("avatar set: 20 creatures, bees first (brand order)") {
    const auto& names = hv::avatarNames();
    REQUIRE(names.size() == 20);
    REQUIRE(names[0] == "queen-bee");
    REQUIRE(names[1] == "worker-bee");
    REQUIRE(names[2] == "drone-bee");
    REQUIRE(names[3] == "honey-bee");
    REQUIRE(names[4] == "bumblebee");
    REQUIRE(names.back() == "cricket");
}

TEST_CASE("auto avatar: stable name-hash, matches the other clients") {
    REQUIRE(hv::autoAvatar("Chief") == "drone-bee");
    REQUIRE(hv::autoAvatar("Scout") == "worm");
    REQUIRE(hv::autoAvatar("HoneyBee") == "beetle");
    REQUIRE(hv::autoAvatar("Bob") == "queen-bee");
    // Deterministic across calls.
    REQUIRE(hv::autoAvatar("Chief") == hv::autoAvatar("Chief"));
}

TEST_CASE("auto avatar: always a member of the set") {
    const auto& names = hv::avatarNames();
    for (const char* n : {"", "a", "Z", "agent-42", "Büro 🐝"})
        REQUIRE(std::find(names.begin(), names.end(), hv::autoAvatar(n)) !=
                names.end());
}

TEST_CASE("resolve: without the gresource registered, falls back to user") {
    // The test binary does not register the app's GResource bundle, so
    // every resourceExists() probe fails and the chain must bottom out
    // at "user" — never crash, never return "".
    REQUIRE(hv::resolveAvatar("Chief", "") == "user");
    REQUIRE(hv::resolveAvatar("Chief", "executive") == "user");
    REQUIRE(hv::resolveAvatar("Chief", "queen-bee") == "user");
}
