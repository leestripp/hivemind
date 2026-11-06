#include "../src/SpeechText.h"

#include <catch2/catch_test_macros.hpp>

using namespace speech;

TEST_CASE("splitClauses: plain sentence", "[speech]") {
    auto c = splitClauses("Hello world. How are you?");
    REQUIRE(c.size() == 2);
    REQUIRE(c[0].terminal);
    REQUIRE(c[0].text == "Hello world.");
    REQUIRE(c[1].text == "How are you?");
}

TEST_CASE("splitClauses: code fence dropped", "[speech]") {
    auto c = splitClauses("Before.\n```cpp\nint x = 1;\n```\nAfter.");
    auto s = toSentences(c);
    REQUIRE(s.size() == 2);
    REQUIRE(s[0] == "Before.");
    REQUIRE(s[1] == "After.");
}

TEST_CASE("splitClauses: bare URL skipped", "[speech]") {
    auto c = splitClauses("See https://example.com/x for details.");
    auto s = toSentences(c);
    REQUIRE(s.size() == 1);
    REQUIRE(s[0] == "See for details.");
}

TEST_CASE("splitClauses: digits expand to words", "[speech]") {
    auto c = splitClauses("I have 42 apples and 7 oranges.");
    auto s = toSentences(c);
    REQUIRE(s.size() == 1);
    REQUIRE(s[0].find("forty two") != std::string::npos);
    REQUIRE(s[0].find("seven") != std::string::npos);
}

TEST_CASE("toSentences: comma clause merges until long", "[speech]") {
    auto c = splitClauses("one, two, three.");
    auto s = toSentences(c);
    REQUIRE(s.size() == 1);
    REQUIRE(s[0] == "one two three.");
}

TEST_CASE("ellipsis becomes a pause", "[speech]") {
    auto c = splitClauses("Well… maybe.");
    auto s = toSentences(c);
    REQUIRE(s.size() == 2);
    REQUIRE(s[0].find("...") != std::string::npos);
}

