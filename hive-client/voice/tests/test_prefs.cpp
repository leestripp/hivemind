#include "../src/Prefs.h"

#include <catch2/catch_test_macros.hpp>

#include <cstdlib>
#include <filesystem>
#include <unistd.h>

TEST_CASE("prefs round trip", "[prefs]") {
    // Redirect HOME to a temp dir so the real config is untouched.
    const auto tmp = std::filesystem::temp_directory_path() /
                     ("hcv-prefs-" + std::to_string(::getpid()));
    std::filesystem::create_directories(tmp);
    ::setenv("HOME", tmp.c_str(), 1);

    Prefs p;
    p.setBase("http://hive.example:8090");
    p.setApiKey("secret");
    p.setAgentId("agent-1");
    p.setAgentName("Scout");
    p.setOnboardingDone(true);
    p.setVoiceFor("agent-1", "en_US-lessac-medium");
    p.save();

    Prefs q;
    q.load();
    REQUIRE(q.base() == "http://hive.example:8090");
    REQUIRE(q.apiKey() == "secret");
    REQUIRE(q.agentId() == "agent-1");
    REQUIRE(q.agentName() == "Scout");
    REQUIRE(q.onboardingDone());
    REQUIRE(q.voiceFor("agent-1") == "en_US-lessac-medium");
    REQUIRE(q.voiceFor("nope").empty());

    std::filesystem::remove_all(tmp);
}
