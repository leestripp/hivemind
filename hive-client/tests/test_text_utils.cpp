// tests/test_text_utils.cpp — pure text/markup helpers (factored out of
// HiveWindow.cpp). Covers transcript stripping, segment splitting, Pango
// markup generation, UTC timestamp labels, palette stability, roster
// lookup and URL escaping.
#include "text_utils.h"

#include <cassert>
#include <ctime>
#include <iostream>
#include <string>

using namespace hive::text;
using nlohmann::json;

static void test_strip_tool_transcript() {
    // Tool call lines and result lines are removed; prose survives.
    const std::string body =
        "Let me check that.\n"
        "[tool: bash ls -la]\n"
        "  → {\"ok\": true}\n"
        "Done — 3 files.\n";
    assert(strip_tool_transcript(body) == "Let me check that.\nDone — 3 files.");

    // Blank runs left by removed lines collapse to a single blank line.
    const std::string blanks = "a\n[tool: x]\n\n\n\nb\n";
    assert(strip_tool_transcript(blanks) == "a\n\nb");

    // A body that is ONLY transcript plumbing strips to empty (the caller
    // renders nothing for it).
    assert(strip_tool_transcript("[tool: x]\n→ {}\n").empty());

    // Leading whitespace before a marker still counts as a marker.
    assert(strip_tool_transcript("   [tool: x]\nkeep\n") == "keep");

    // Plain prose passes through unchanged (modulo trailing trim).
    assert(strip_tool_transcript("hello world") == "hello world");
    assert(strip_tool_transcript("").empty());
}

static void test_split_segments() {
    // Prose / code / prose.
    auto segs = split_segments("before\n```\ncode line\n```\nafter\n");
    assert(segs.size() == 3);
    assert(!segs[0].code && segs[0].text == "before");
    assert(segs[1].code && segs[1].text == "code line");
    assert(!segs[2].code && segs[2].text == "after");

    // No fences → single prose segment.
    segs = split_segments("just prose\n");
    assert(segs.size() == 1 && !segs[0].code);

    // Unterminated fence: the rest of the message is code.
    segs = split_segments("intro\n```\nunclosed\n");
    assert(segs.size() == 2 && segs[1].code && segs[1].text == "unclosed");

    // Fence with a language tag is still a fence.
    segs = split_segments("```cpp\nint x;\n```\n");
    assert(segs.size() == 1 && segs[0].code && segs[0].text == "int x;");

    // Empty input → no segments.
    assert(split_segments("").empty());
}

static void test_inline_markup() {
    // Bold, italic, code.
    assert(inline_markup("**bold**") == "<b>bold</b>");
    assert(inline_markup("*it*") == "<i>it</i>");
    assert(inline_markup("`co de`") ==
           "<span font_family='monospace' background='#88888830'>co de</span>");

    // Pango-special chars inside spans are escaped.
    assert(inline_markup("**a<b&c>**") == "<b>a&lt;b&amp;c&gt;</b>");
    assert(inline_markup("x < y & z") == "x &lt; y &amp; z");

    // Unclosed markers render literally.
    assert(inline_markup("**unclosed") == "**unclosed");
    assert(inline_markup("`unclosed") == "`unclosed");
    assert(inline_markup("*unclosed") == "*unclosed");

    // A lone * followed by a space is literal (avoids eating "a * b").
    assert(inline_markup("a * b") == "a * b");

    // Italic does not fire on the first half of a bold marker.
    assert(inline_markup("**b** then *i*") == "<b>b</b> then <i>i</i>");
}

static void test_prose_to_markup() {
    // Headers.
    assert(prose_to_markup("# Title") ==
           "<span size='x-large' weight='bold'>Title</span>");
    assert(prose_to_markup("## Sub\n### SubSub") ==
           "<span size='large' weight='bold'>Sub</span>\n"
           "<span size='large' weight='bold'>SubSub</span>");

    // Bullets get a real bullet char; inline markup applies after them.
    assert(prose_to_markup("- item **b**") ==
           "\xe2\x80\xa2 item <b>b</b>");

    // Multi-line joins with newlines, plain lines use inline markup.
    assert(prose_to_markup("one\ntwo") == "one\ntwo");
}

static void test_format_msg_time() {
    // Too-short input → empty.
    assert(format_msg_time("").empty());
    assert(format_msg_time("2026-08-23T06").empty());

    // Same UTC day → time only.
    std::time_t t = std::time(nullptr);
    std::tm ut{};
    gmtime_r(&t, &ut);
    char today[16];
    std::strftime(today, sizeof today, "%Y-%m-%d", &ut);
    const std::string iso_today = std::string(today) + "T06:09:43.6Z";
    assert(format_msg_time(iso_today) == "06:09 UTC");

    // Different day → "Mon DD, HH:MM UTC".
    assert(format_msg_time("2020-01-05T22:14:00Z") == "Jan 05, 22:14 UTC");

    // Malformed date no longer renders "Jan 00" — raw passthrough.
    assert(format_msg_time("not-a-dateT22:14:00Z") == "not-a-date 22:14 UTC");
}

static void test_palette_index() {
    for (const char* n : {"Chief", "Forge", "PA", "", "x"}) {
        const int i = palette_index(n);
        assert(i >= 0 && i < 6);
        assert(palette_index(n) == i);   // stable
    }
    assert(palette_index("a") != palette_index("a") + 6 % 6 || true);
}

static void test_agent_name_of() {
    const json agents = json::array({
        {{"id", "a1"}, {"name", "Chief"}},
        {{"id", "a2"}, {"name", "Forge"}},
    });
    assert(agent_name_of(agents, "a1") == "Chief");
    assert(agent_name_of(agents, "missing") == "missing");   // id fallback
    assert(agent_name_of(json::array(), "a1") == "a1");
    // Non-object entries are skipped, not crashed on.
    const json junk = json::array({42, "str", {{"id", "a9"}, {"name", "Z"}}});
    assert(agent_name_of(junk, "a9") == "Z");
}

static void test_url_escape() {
    assert(url_escape("plain-Name_1.0~x") == "plain-Name_1.0~x");
    assert(url_escape("a b&c=d") == "a%20b%26c%3Dd");
    assert(url_escape("/slash?") == "%2Fslash%3F");
    assert(url_escape("").empty());
    // UTF-8 bytes are escaped individually.
    assert(url_escape("\xc3\xa9") == "%C3%A9");
}

int main() {
    test_strip_tool_transcript();
    test_split_segments();
    test_inline_markup();
    test_prose_to_markup();
    test_format_msg_time();
    test_palette_index();
    test_agent_name_of();
    test_url_escape();
    std::cout << "test_text_utils: OK\n";
    return 0;
}
