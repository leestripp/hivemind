#include "../src/Prefs.h"

#include <catch2/catch_test_macros.hpp>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

TEST_CASE("prefs round trip", "[prefs]") {
    // Redirect HOME to a temp dir so the real config is untouched.
    const auto tmp = std::filesystem::temp_directory_path() /
                     ("hcv-prefs-" + std::to_string(::getpid()));
    std::filesystem::create_directories(tmp);
    ::setenv("HOME", tmp.c_str(), 1);
    ::unsetenv("CHIMPFLOW_URL");
    ::unsetenv("CHIMPFLOW_KEY");

    // Saving must tighten permissions even when the key file already exists.
    const auto key_path = tmp / ".config/hive-client-voice/chimpflow.key";
    std::filesystem::create_directories(key_path.parent_path());
    std::ofstream{key_path} << "old-secret\n";
    REQUIRE(::chmod(key_path.c_str(), 0644) == 0);

    Prefs p;
    p.setBase("http://hive.example:8090");
    p.setApiKey("secret");
    p.setAgentId("agent-1");
    p.setAgentName("Scout");
    p.setOnboardingDone(true);
    p.setVoiceFor("agent-1", "en_US-lessac-medium");
    p.setChimpflowUrl("http://127.0.0.1:8090");
    p.setChimpflowKey("chimp-secret");
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
    REQUIRE(q.chimpflowUrl() == "http://127.0.0.1:8090");
    REQUIRE(q.chimpflowKey() == "chimp-secret");

    std::ifstream json_file(Prefs::configPath());
    const std::string json{std::istreambuf_iterator<char>{json_file},
                           std::istreambuf_iterator<char>{}};
    REQUIRE(json.find("chimp-secret") == std::string::npos);

    struct stat info {};
    REQUIRE(::stat(key_path.c_str(), &info) == 0);
    REQUIRE((info.st_mode & 0777) == 0600);

    std::filesystem::remove_all(tmp);
}

TEST_CASE("ChimpFlow endpoint parsing", "[prefs][chimpflow]") {
    using voice_settings::parseEndpoint;
    const auto local = parseEndpoint("http://127.0.0.1:8090");
    REQUIRE(local.address == "127.0.0.1");
    REQUIRE(local.port == "8090");
    const auto named = parseEndpoint("http://localhost");
    REQUIRE(named.address == "localhost");
    REQUIRE(named.port == "8090");
    const auto ipv6 = parseEndpoint("http://[::1]:9000");
    REQUIRE(ipv6.address == "[::1]");
    REQUIRE(ipv6.port == "9000");
}

TEST_CASE("ChimpFlow Settings accepts loopback endpoints only",
          "[prefs][chimpflow]") {
    using voice_settings::makeLoopbackUrl;
    REQUIRE(makeLoopbackUrl("127.0.0.1", "8090") ==
            "http://127.0.0.1:8090");
    REQUIRE(makeLoopbackUrl(" localhost ", " 9000 ") ==
            "http://localhost:9000");
    REQUIRE(makeLoopbackUrl("::1", "8090") == "http://[::1]:8090");
    REQUIRE_THROWS(makeLoopbackUrl("192.0.2.10", "8090"));
    REQUIRE_THROWS(makeLoopbackUrl("127.0.0.1", "not-a-port"));
    REQUIRE_THROWS(makeLoopbackUrl("127.0.0.1", "0"));
    REQUIRE_THROWS(makeLoopbackUrl("127.0.0.1", "65536"));
}
