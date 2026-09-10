// SPDX-License-Identifier: BSL-1.0
// tests/test_think_tag_filter.cpp
//
// §ThinkTagFilter (2026-08-01): unit tests for the stream-safe inline
// <think>...</think> stripper (src/api/ThinkTagFilter.h). Hosts reported
// stray </think> tags leaking into UIs: models that embed reasoning inline
// in `content` (qwen3-family, MiniMax without honored reasoning_split,
// deepseek-style) had their tags passed verbatim through the shared SSE
// parser, and chunk-split tags defeat app-level string replacement.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <string>
#include <string_view>

#include "api/ThinkTagFilter.h"

namespace {

// Feed every piece through one filter and concatenate the visible output
// plus the flush tail — mimics the parser's delta loop + finish().
std::string run(std::initializer_list<std::string_view> pieces) {
    ThinkTagFilter f;
    std::string out;
    for (const auto p : pieces) out += f.feed(p);
    out += f.flush();
    return out;
}

} // namespace

TEST_CASE("clean text passes through unchanged") {
    CHECK(run({"hello world"}) == "hello world");
    CHECK(run({"hello ", "wor", "ld"}) == "hello world");
    CHECK(run({""}) == "");
}

TEST_CASE("complete think block in one chunk is dropped") {
    CHECK(run({"<think>secret reasoning</think>visible"}) == "visible");
    CHECK(run({"before <think>secret</think> after"}) == "before  after");
    CHECK(run({"<think>only reasoning</think>"}) == "");
}

TEST_CASE("tags split across chunks are still stripped") {
    CHECK(run({"<th", "ink>secret</th", "ink>visible"}) == "visible");
    CHECK(run({"<", "t", "h", "i", "n", "k", ">", "x",
               "<", "/", "t", "h", "i", "n", "k", ">", "y"}) == "y");
    CHECK(run({"abc</", "think>def"}) == "abcdef");  // stray close, split
}

TEST_CASE("stray </think> without an opener strips just the tag") {
    // The reported symptom: providers that open reasoning implicitly (or
    // stream it via reasoning_content) but still emit the closing tag.
    // Text before the tag was already streamed live — it stays; only the
    // tag itself is stripped.
    CHECK(run({"reasoning-ish</think>visible"}) == "reasoning-ishvisible");
    CHECK(run({"</think>The reply."}) == "The reply.");
    CHECK(run({"a</think>b</think>c"}) == "abc");
}

TEST_CASE("multiple think blocks") {
    CHECK(run({"<think>a</think>one<think>b</think>two"}) == "onetwo");
}

TEST_CASE("unterminated think block drops the tail at flush") {
    CHECK(run({"visible<think>never closed"}) == "visible");
    // A *partial* opener cut by end-of-stream never became a tag — the
    // held bytes come back as literal text (never drop unconfirmed text).
    CHECK(run({"visible<think"}) == "visible<think");
}

TEST_CASE("held bytes that are not a tag are re-emitted") {
    CHECK(run({"ends with<"}) == "ends with<");
    CHECK(run({"a<th", "x>b"}) == "a<thx>b");          // <thx> is not a tag
    CHECK(run({"<thinking>not a tag</thinking>"}) ==
              "<thinking>not a tag</thinking>");
    CHECK(run({"< think>spaced</think>"}) == "< think>spaced");
    // ^ "< think>" is not the opener, but "</think>" IS a stray closer.
}

TEST_CASE("literal '<' inside a think block is dropped with the block") {
    CHECK(run({"<think>a < b</think>out"}) == "out");
}

TEST_CASE("one-shot strip() matches streamed filtering") {
    CHECK(ThinkTagFilter::strip("plain") == "plain");
    CHECK(ThinkTagFilter::strip("<think>r</think>v") == "v");
    CHECK(ThinkTagFilter::strip("a</think>b") == "ab");
    CHECK(ThinkTagFilter::strip("<think>unterminated") == "");
    CHECK(ThinkTagFilter::strip("") == "");
}
