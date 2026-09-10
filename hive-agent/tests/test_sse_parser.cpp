/**
 * @file tests/test_sse_parser.cpp
 * @brief Unit tests for hive::net::SseParser (§NC-2)
 *
 * Coverage:
 *   - Basic event parsing (single line each: id, event, data, retry)
 *   - Multi-line `data` field joining
 *   - CRLF and LF line endings
 *   - Comments (lines starting with ':')
 *   - Multiple events in one chunk
 *   - Events split across chunk boundaries
 *   - `finish()` flushes a trailing in-flight event
 *   - Events without `data` are skipped (per spec)
 *   - Unknown fields are silently ignored
 *   - retry field only parses if all chars are digits
 *   - Leading space after ':' is stripped
 */

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "net/SseParser.h"

using hive::net::SseEvent;
using hive::net::SseParser;

namespace {
void parse_one(const std::string& chunk,
               std::vector<SseEvent>& out,
               bool finish = false) {
    SseParser p;
    p.feed(chunk);
    if (finish) p.finish();
    while (auto ev = p.try_pop()) out.push_back(std::move(*ev));
}
} // namespace

TEST_CASE("SseParser: basic single-line event") {
    std::vector<SseEvent> out;
    parse_one("data: hello world\n\n", out);
    REQUIRE(out.size() == 1);
    CHECK(out[0].data == "hello world");
    CHECK(out[0].event.empty());   // defaults to "message"
    CHECK(out[0].id.empty());
    CHECK(out[0].retry == 0);
}

TEST_CASE("SseParser: all four fields") {
    std::vector<SseEvent> out;
    parse_one(
        "id: 42\nevent: tick\ndata: hello\nretry: 3000\n\n",
        out);
    REQUIRE(out.size() == 1);
    CHECK(out[0].id    == "42");
    CHECK(out[0].event == "tick");
    CHECK(out[0].data  == "hello");
    CHECK(out[0].retry == 3000);
}

TEST_CASE("SseParser: multi-line data joined with \\n") {
    std::vector<SseEvent> out;
    parse_one(
        "data: line1\ndata: line2\ndata: line3\n\n",
        out);
    REQUIRE(out.size() == 1);
    CHECK(out[0].data == "line1\nline2\nline3");
}

TEST_CASE("SseParser: CRLF line endings") {
    std::vector<SseEvent> out;
    parse_one("data: hello\r\n\r\n", out);
    REQUIRE(out.size() == 1);
    CHECK(out[0].data == "hello");
}

TEST_CASE("SseParser: mixed CRLF/LF endings") {
    std::vector<SseEvent> out;
    parse_one("data: a\r\ndata: b\n\n", out);
    REQUIRE(out.size() == 1);
    CHECK(out[0].data == "a\nb");
}

TEST_CASE("SseParser: comment lines ignored") {
    std::vector<SseEvent> out;
    parse_one(
        ": this is a heartbeat\ndata: real\n: another comment\n\n",
        out);
    REQUIRE(out.size() == 1);
    CHECK(out[0].data == "real");
}

TEST_CASE("SseParser: multiple events in one chunk") {
    std::vector<SseEvent> out;
    parse_one(
        "data: first\n\n"
        "data: second\n\n"
        "data: third\n\n",
        out);
    REQUIRE(out.size() == 3);
    CHECK(out[0].data == "first");
    CHECK(out[1].data == "second");
    CHECK(out[2].data == "third");
}

TEST_CASE("SseParser: event split across chunk boundaries") {
    std::vector<SseEvent> out;
    SseParser p;

    p.feed("da");        // partial line
    CHECK(p.try_pop() == std::nullopt);

    p.feed("ta: hello\n"); // completes the line
    CHECK(p.try_pop() == std::nullopt);  // no terminator yet

    p.feed("\n");        // blank line — flushes event
    auto ev = p.try_pop();
    REQUIRE(ev.has_value());
    CHECK(ev->data == "hello");
}

TEST_CASE("SseParser: finish() flushes trailing event without terminator") {
    SseParser p;
    p.feed("data: trailing\n");
    REQUIRE(p.try_pop() == std::nullopt);  // no blank line yet
    p.finish();
    auto ev = p.try_pop();
    REQUIRE(ev.has_value());
    CHECK(ev->data == "trailing");
}

TEST_CASE("SseParser: finish() with no trailing event is a no-op") {
    SseParser p;
    p.feed("data: a\n\n");
    p.try_pop();                         // drain the event
    p.finish();
    CHECK(p.try_pop() == std::nullopt);
}

TEST_CASE("SseParser: events without `data` field are skipped (per spec)") {
    std::vector<SseEvent> out;
    parse_one(
        "event: heartbeat\nid: 1\n\n"
        "data: real\n\n",
        out);
    REQUIRE(out.size() == 1);
    CHECK(out[0].data == "real");
}

TEST_CASE("SseParser: unknown fields are silently ignored") {
    std::vector<SseEvent> out;
    parse_one(
        "weird: stuff\ndata: ok\nmore: garbage\n\n",
        out);
    REQUIRE(out.size() == 1);
    CHECK(out[0].data == "ok");
}

TEST_CASE("SseParser: retry field only parses if all digits") {
    std::vector<SseEvent> out;
    parse_one("retry: 5000\ndata: a\n\n", out);
    REQUIRE(out.size() == 1);
    CHECK(out[0].retry == 5000);

    out.clear();
    parse_one("retry: 5ms\ndata: b\n\n", out);
    REQUIRE(out.size() == 1);
    CHECK(out[0].retry == 0);   // rejected — has non-digits
}

TEST_CASE("SseParser: leading space after ':' is stripped (exactly one)") {
    // Spec: a single leading space after the colon is stripped; any
    // additional leading spaces are part of the value.
    std::vector<SseEvent> out;
    parse_one("data:    leading space\n\n", out);
    REQUIRE(out.size() == 1);
    CHECK(out[0].data == "   leading space");   // one of four spaces stripped
}

TEST_CASE("SseParser: data without leading space is preserved verbatim") {
    std::vector<SseEvent> out;
    parse_one("data:nospace\n\n", out);
    REQUIRE(out.size() == 1);
    CHECK(out[0].data == "nospace");
}

TEST_CASE("SseParser: field with no value (no colon) is treated as empty") {
    std::vector<SseEvent> out;
    parse_one("data\n\n", out);
    REQUIRE(out.size() == 1);
    CHECK(out[0].data == "");
}

TEST_CASE("SseParser: idle() reflects state") {
    SseParser p;
    CHECK(p.idle());
    p.feed("data: x\n\n");
    CHECK_FALSE(p.idle());      // event buffered, not yet popped
    p.try_pop();
    CHECK(p.idle());
}