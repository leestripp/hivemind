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

TEST_CASE("toSpeechChunks: preserves sentence queueing", "[speech]") {
    const auto chunks = toSpeechChunks("One. Two. Three.", 6);
    REQUIRE(chunks == std::vector<std::string>{"One.", "Two.", "Three."});
}

TEST_CASE("toSpeechChunks: splits oversized sentences at words", "[speech]") {
    const auto chunks = toSpeechChunks("aaaa bbbb cccc", 11);
    REQUIRE(chunks == std::vector<std::string>{"aaaa bbbb", "cccc"});
}

TEST_CASE("toSpeechChunks: never cuts a UTF-8 sequence", "[speech]") {
    std::string original;
    for (int i = 0; i < 12; ++i) original += "\u00e9";
    const auto chunks = toSpeechChunks(original, 7);
    std::string rejoined;
    for (const auto& chunk : chunks) {
        REQUIRE(!chunk.empty());
        REQUIRE((static_cast<unsigned char>(chunk.front()) & 0xc0) != 0x80);
        rejoined += chunk;
    }
    REQUIRE(rejoined == original);
}

TEST_CASE("toSpeechChunks: zero budget produces no work", "[speech]") {
    REQUIRE(toSpeechChunks("Readable text.", 0).empty());
}
