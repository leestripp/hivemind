#include <catch2/catch_test_macros.hpp>

#include "TranscriptText.h"

TEST_CASE("multi-sentence STT remains one unchanged message", "[transcript]") {
    const std::string input = "First request. Second request? Third request!\nFourth request.";
    REQUIRE(hv::sanitizeSttTranscript(input) == input);
}

TEST_CASE("silence artifacts never become a user message", "[transcript]") {
    REQUIRE(hv::sanitizeSttTranscript("[BLANK_AUDIO]").empty());
    REQUIRE(hv::sanitizeSttTranscript(" (silence) ").empty());
    REQUIRE(hv::sanitizeSttTranscript(" \n ").empty());
}

TEST_CASE("artifact removal still returns one complete message", "[transcript]") {
    REQUIRE(hv::sanitizeSttTranscript(
        "First request.\n[BLANK_AUDIO]\nSecond request? (silence)\nThird request!") ==
        "First request. Second request? Third request!");
}
