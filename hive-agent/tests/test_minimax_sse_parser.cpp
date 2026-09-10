/**
 * @file tests/test_minimax_sse_parser.cpp
 * @brief BUG-001 (2026-07-17) — unit tests for MiniMaxSseParser +
 *        is_usage_only_chunk() gate.
 *
 * The original LLMClient streaming-path gate keyed on
 * `!response_json["usage"].is_null()`. MiniMax's API sends
 * `"usage": null` on the final chunk, which made the gate
 * fail and threw away a successfully-streamed response into
 * a 7-second silent retry spin.
 *
 * The fix moves the gate logic to a free function
 * `is_usage_only_chunk()` in MiniMaxSseParser.h. It keys on
 * what the SSE parser has accumulated, not what `usage`
 * contains. This file tests that gate with the truth table
 * from the bug report.
 *
 * Not part of the public API — uses the private header
 * `src/api/MiniMaxSseParser.h`.
 */

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "api/MiniMaxSseParser.h"

#include <nlohmann/json.hpp>

#include <memory>
#include <string>

using nlohmann::json;

// Helper: construct a MiniMaxSseParser with a null sink (no on_assistant_delta
// callbacks expected). The parser does not crash on null sink — its on_chunk()
// only fires `weak_sink_.lock()` which returns nullptr in that case.
static MiniMaxSseParser make_parser() {
    return MiniMaxSseParser{std::weak_ptr<hive::AgentSink>{}};
}

// Helper: feed a single SSE-formatted chunk that wraps the given JSON object.
// MiniMax SSE wire format is "data: <json>\n\n".
static void feed(MiniMaxSseParser& p, const json& obj) {
    p.on_chunk("data: " + obj.dump() + "\n\n");
}

// Helper: feed a text delta SSE chunk.
// Builds the JSON directly as a raw string then parses it, avoiding
// complex C++11 initializer-list nesting that is hard to get right.
static void feed_text_delta(MiniMaxSseParser& p, const std::string& s) {
    nlohmann::json obj = nlohmann::json::parse(
        "{\"id\":\"chunk-1\",\"choices\":[{\"index\":0,"
        "\"delta\":{\"role\":\"assistant\",\"content\":" + nlohmann::json(s).dump() + "}}]}"
    );
    feed(p, obj);
}

// Helper: feed a tool_call delta SSE chunk (MiniMax streams the whole
// tool_call object in one chunk).
static void feed_tool_call_delta(MiniMaxSseParser& p, const std::string& id,
                                  const std::string& name,
                                  const std::string& arguments) {
    nlohmann::json obj = nlohmann::json::parse(
        "{\"id\":\"chunk-tc\",\"choices\":["
        "{\"index\":0,\"delta\":"
        "{\"role\":\"assistant\","
        "\"tool_calls\":[{"
        "\"index\":0,"
        "\"id\":" + nlohmann::json(id).dump() + ","
        "\"type\":\"function\","
        "\"function\":{"
        "\"name\":" + nlohmann::json(name).dump() + ","
        "\"arguments\":" + nlohmann::json(arguments).dump() +
        "}}]}}]}"
    );
    feed(p, obj);
}

// Helper: feed a "usage-only trailer" — the SSE chunk that MiniMax
// sends AFTER all the deltas. choices is [], usage is null.
static void feed_usage_only_trailer(MiniMaxSseParser& p) {
    feed(p, json{{"id", "chunk-final"},
                  {"choices", json::array()},
                  {"usage", nullptr}});
}

// Helper: feed a "usage-only trailer" with non-null usage (OpenAI's shape).
static void feed_usage_only_trailer_non_null(MiniMaxSseParser& p) {
    feed(p, json{{"id", "chunk-final"},
                  {"choices", json::array()},
                  {"usage", {{"prompt_tokens", 11},
                             {"completion_tokens", 22},
                             {"total_tokens", 33}}}});
}

// =====================================================================
// BUG-001: truth-table for is_usage_only_chunk()
// =====================================================================

TEST_CASE("BUG-001 gate: empty parser + empty response -> not usage-only (recoverable)") {
    MiniMaxSseParser p = make_parser();
    p.finish();
    json empty = json::object();
    CHECK_FALSE(is_usage_only_chunk(/*stream=*/true, empty, p));
    CHECK_FALSE(is_usage_only_chunk(/*stream=*/false, empty, p));
}

TEST_CASE("BUG-001 gate: stream disabled -> not usage-only") {
    MiniMaxSseParser p = make_parser();
    p.finish();
    json response = json{{"choices", json::array()}, {"usage", nullptr}};
    CHECK_FALSE(is_usage_only_chunk(/*stream=*/false, response, p));
}

TEST_CASE("BUG-001 gate: text deltas accumulated + usage=null -> usage-only (BUG-001)") {
    // The bug's primary symptom: assistant text was streamed through
    // on_assistant_delta, but the final usage-only trailer (MiniMax's
    // shape: choices=[], usage=null) made the gate return false and
    // route the response into the 3-retry retry path. NEW code: gate
    // succeeds (parser.had_text_deltas()), recovered.
    MiniMaxSseParser p = make_parser();
    feed_text_delta(p, "Hello, world!");
    feed_usage_only_trailer(p);
    p.finish();
    json response = json{{"choices", json::array()}, {"usage", nullptr}};

    CHECK(p.had_text_deltas());
    CHECK(p.accumulated_tool_calls().empty());
    CHECK(is_usage_only_chunk(/*stream=*/true, response, p));
}

TEST_CASE("BUG-001 gate: tool calls accumulated + usage=null -> usage-only (BUG-001)") {
    // The bug also breaks tool-call responses. A streamed tool_call
    // gets thrown away, so the agent forgets what tool it was about
    // to invoke.
    MiniMaxSseParser p = make_parser();
    feed_tool_call_delta(p, "call_abc", "bash",
                          R"({"command":"ls -la"})");
    feed_usage_only_trailer(p);
    p.finish();
    json response = json{{"choices", json::array()}, {"usage", nullptr}};

    CHECK_FALSE(p.had_text_deltas());
    CHECK_FALSE(p.accumulated_tool_calls().empty());
    CHECK(is_usage_only_chunk(/*stream=*/true, response, p));
    // The synthesized message should now contain the recovered tool call.
    json synth = p.synthesized_message();
    REQUIRE(synth.contains("tool_calls"));
    REQUIRE(synth["tool_calls"].is_array());
    REQUIRE(synth["tool_calls"].size() == 1);
    CHECK(synth["tool_calls"][0]["id"] == "call_abc");
    CHECK(synth["tool_calls"][0]["function"]["name"] == "bash");
}

TEST_CASE("BUG-001 gate: text + tool calls + usage=null -> usage-only") {
    // Both text and tool_calls present (e.g. assistant emits a sentence
    // then calls a tool).
    MiniMaxSseParser p = make_parser();
    feed_text_delta(p, "Let me check: ");
    feed_tool_call_delta(p, "call_xyz", "bash",
                          R"({"command":"pwd"})");
    feed_usage_only_trailer(p);
    p.finish();

    CHECK(p.had_text_deltas());
    CHECK_FALSE(p.accumulated_tool_calls().empty());
    json response = json{{"choices", json::array()}, {"usage", nullptr}};
    CHECK(is_usage_only_chunk(/*stream=*/true, response, p));
}

TEST_CASE("BUG-001 gate: OpenAI shape (usage=non-null) + text deltas -> usage-only") {
    // The original (working) case: OpenAI sends usage as a real object
    // on the trailer. Gate still returns true (regression check).
    MiniMaxSseParser p = make_parser();
    feed_text_delta(p, "Hello");
    feed_usage_only_trailer_non_null(p);
    p.finish();
    json response = json{{"choices", json::array()},
                         {"usage", {{"prompt_tokens", 1},
                                    {"completion_tokens", 2},
                                    {"total_tokens", 3}}}};
    CHECK(is_usage_only_chunk(/*stream=*/true, response, p));
}

TEST_CASE("BUG-001 gate: response has choices -> not usage-only") {
    // The gate should bail out if `choices` is non-empty. We never get
    // here in practice (the outer `if` blocks it), but the gate should
    // be defensive.
    MiniMaxSseParser p = make_parser();
    feed_text_delta(p, "Hello");
    p.finish();
    json response = json{{"choices", json::array({{
        {"index", 0},
        {"message", {{"role", "assistant"}, {"content", "Hi"}}}
    }})}};
    CHECK_FALSE(is_usage_only_chunk(/*stream=*/true, response, p));
}

TEST_CASE("BUG-001 gate: no streaming, no chunks -> not usage-only") {
    // Empty parser + no streaming = nothing to do.
    MiniMaxSseParser p = make_parser();
    p.finish();
    json empty = json::object();
    CHECK_FALSE(is_usage_only_chunk(/*stream=*/true, empty, p));
    CHECK_FALSE(is_usage_only_chunk(/*stream=*/false, empty, p));
}

// =====================================================================
// BUG-001: recovered tool calls land in synthesized_message() correctly
// =====================================================================

TEST_CASE("BUG-001 recovery: tool_call + usage=null trailer -> synthesized_message has tool_call") {
    // The fix path: gate triggers, synthesized_message() is rebuilt,
    // downstream code (messages.push_back) sees a valid tool_call.
    MiniMaxSseParser p = make_parser();
    feed_tool_call_delta(p, "call_1", "bash", R"({"cmd":"ls"})");
    feed_usage_only_trailer(p);
    p.finish();

    json synth = p.synthesized_message();
    CHECK(synth["role"] == "assistant");
    CHECK(synth["content"] == "");
    CHECK(synth.contains("tool_calls"));
    CHECK(synth["tool_calls"].size() == 1);
    CHECK(synth["tool_calls"][0]["function"]["name"] == "bash");
}

TEST_CASE("BUG-001 regression: text + usage=null trailer -> still routes to recovery path") {
    // Wait - this case should ALSO recover now. Let me re-verify the gate
    // returns true here too (text-only case is the original bug).
    MiniMaxSseParser p = make_parser();
    feed_text_delta(p, "I will check this for you.");
    feed_usage_only_trailer(p);
    p.finish();

    json response = json{{"choices", json::array()}, {"usage", nullptr}};
    // The fix: gate is true (text was accumulated), so the response
    // is recovered, NOT routed to retry.
    CHECK(is_usage_only_chunk(/*stream=*/true, response, p));
    json synth = p.synthesized_message();
    CHECK(synth["content"] == "I will check this for you.");
}

// =====================================================================
// BUG-001: malformed chunks (defensive — must not crash the gate)
// =====================================================================

TEST_CASE("BUG-001 gate: garbage response_json (no 'choices' or 'usage' keys) + empty parser") {
    MiniMaxSseParser p = make_parser();
    p.finish();
    // Some servers might omit both keys. Gate should return false (no
    // recovered response) — caller will retry.
    json garbage = json{{"id", "chunk-1"}, {"object", "chat.completion.chunk"}};
    CHECK_FALSE(is_usage_only_chunk(/*stream=*/true, garbage, p));
}

TEST_CASE("BUG-001 gate: response_json has unexpected shape") {
    // E.g. some providers send a completion object instead of a chunk.
    MiniMaxSseParser p = make_parser();
    feed_text_delta(p, "Hi");
    p.finish();
    json weird = json{{"id", "x"}, {"completion", "Hi"}, {"choices", json::array()}};
    // The gate depends on the parser, not the response_json shape, so
    // it correctly returns true here (text was accumulated).
    CHECK(is_usage_only_chunk(/*stream=*/true, weird, p));
}

// =====================================================================
// §fix (2026-07-18): non-object choices must not throw
// =====================================================================
// `data: {"choices":[null]}` made choice.value("delta", ...) throw
// json::type_error, which escaped the old parse_error-only catch onto
// HttpSession's detached streaming thread → std::terminate. The parser
// must skip such entries silently and keep working.

TEST_CASE("non-object choice entries are skipped without throwing") {
    MiniMaxSseParser p = make_parser();
    // null, primitive, and array choices are all malformed but survivable.
    CHECK_NOTHROW(p.on_chunk("data: {\"choices\":[null]}\n\n"));
    CHECK_NOTHROW(p.on_chunk("data: {\"choices\":[42,\"str\",[1]]}\n\n"));
    // A "choices" member that isn't an array at all.
    CHECK_NOTHROW(p.on_chunk("data: {\"choices\":null}\n\n"));
    p.finish();

    // The parser swallowed the garbage and still works afterwards.
    CHECK_FALSE(p.had_text_deltas());
    feed_text_delta(p, "still alive");
    feed_usage_only_trailer(p);
    p.finish();
    CHECK(p.had_text_deltas());
}

// =====================================================================
// §ThinkTagFilter (2026-08-01): inline <think>...</think> stripping
// =====================================================================
// Models that embed reasoning inline in `content` (qwen3-family, MiniMax
// without honored reasoning_split, deepseek-style) had their tags passed
// verbatim to the sink and into history. The parser now strips them —
// including tags split across SSE chunks and stray closing tags.

TEST_CASE("ThinkTagFilter: inline think block stripped from text and history") {
    MiniMaxSseParser p = make_parser();
    feed_text_delta(p, "<think>secret reasoning</think>The answer is 42.");
    p.finish();
    CHECK(p.text() == "The answer is 42.");
    CHECK(p.synthesized_message()["content"] == "The answer is 42.");
}

TEST_CASE("ThinkTagFilter: tags split across SSE chunks") {
    MiniMaxSseParser p = make_parser();
    feed_text_delta(p, "<th");
    feed_text_delta(p, "ink>hidden</th");
    feed_text_delta(p, "ink>visible");
    p.finish();
    CHECK(p.text() == "visible");
}

TEST_CASE("ThinkTagFilter: stray </think> closer is stripped") {
    MiniMaxSseParser p = make_parser();
    feed_text_delta(p, "reasoning leaked");
    feed_text_delta(p, "</think>The reply.");
    p.finish();
    // Text streamed before the stray closer was already delivered live —
    // only the tag itself is stripped.
    CHECK(p.text() == "reasoning leakedThe reply.");
}

TEST_CASE("ThinkTagFilter: reasoning_content field channel is untouched") {
    // Kimi-style reasoning via the dedicated field must still round-trip
    // into synthesized_message() — the inline filter must not eat it.
    MiniMaxSseParser p = make_parser();
    feed(p, json::parse(
        "{\"id\":\"c1\",\"choices\":[{\"index\":0,\"delta\":"
        "{\"role\":\"assistant\",\"reasoning_content\":\"field thinking\","
        "\"content\":\"reply\"}}]}"));
    p.finish();
    CHECK(p.text() == "reply");
    CHECK(p.synthesized_message()["reasoning_content"] == "field thinking");
}
