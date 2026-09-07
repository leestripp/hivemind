// SPDX-License-Identifier: BSL-1.0
// src/api/MiniMaxSseParser.h
//
// Private header for `MiniMaxSseParser` — the SSE line-format parser
// for MiniMax's chat-completions streaming endpoint. Implementation
// lives in src/api/LLMClient.cpp; this header exposes just the
// class so unit tests can construct one and feed it chunks.
//
// Not part of the public libdaedalus API. Do not include from
// include/hive-brain/* or from any header that ends up in a host's
// translation unit.

#pragma once

#include <nlohmann/json.hpp>

#include <memory>
#include <string>
#include <string_view>

#include "api/ProviderPlugin.h"  // §ProviderPlugins: ProviderStreamParser interface
#include "api/ThinkTagFilter.h"       // inline <think>...</think> stripping

class MiniMaxSseParser : public hive::ProviderStreamParser {
public:
    /// Sink is used to fire `on_assistant_delta` callbacks as text
    /// streams in. May be a `std::weak_ptr` (typical — sink lifetime
    /// is shorter than parser's).
    explicit MiniMaxSseParser(std::weak_ptr<hive::AgentSink> sink)
        : weak_sink_(std::move(sink)) {}

    /// Called for each raw HTTP chunk; SSE lines are buffered and parsed.
    void on_chunk(std::string_view data) override;

    /// Flush any incomplete line after the stream ends.
    void finish() override;

    /// Returns all text accumulated from delta events. Used for display.
    const std::string& text() const noexcept { return accumulated_text_; }

    /// Returns the final SSE event as a JSON object. Contains the complete
    /// response metadata (id, model, usage, finish_reason) needed for
    /// token tracking and downstream JSON parsing.
    /// Returns a null json::object() if no valid event was received.
    const nlohmann::json& final_event() const noexcept override { return final_event_; }

    /// Reconstruct the assistant's message in the standard non-streaming
    /// format.  When the server streams `delta`-shaped chunks, the
    /// downstream code that consumes `choices[0].message` breaks unless we
    /// synthesise the equivalent object from accumulated fragments:
    ///   {
    ///     "role": "assistant",
    ///     "content": "<accumulated text>",
    ///     "tool_calls": [<accumulated tool calls>],     // if any
    ///     "audio_content": ""                            // MiniMax quirk
    ///   }
    /// `final_event_` is *not* used here — it is the LAST SSE chunk, which
    /// carries the final `finish_reason` and `usage` but only the trailing
    /// `delta` text fragment.  All reassembly has to come from the deltas
    /// we buffered as they arrived.
    nlohmann::json synthesized_message() const override;

    /// §Phase 9.11: Did the SSE stream emit any text via `on_assistant_delta`
    /// during this turn?  Used by the downstream pipeline to decide whether
    /// to re-emit the synthesized message's content (which would duplicate
    /// every fragment the user already saw streaming live).
    bool had_text_deltas() const noexcept override { return !accumulated_text_.empty(); }

    /// §BUG-001 (2026-07-17): did the SSE stream accumulate any tool calls?
    /// Returns the (possibly-empty) array of accumulated tool-call objects.
    /// Used by the gate condition that decides whether a final SSE chunk
    /// with `choices: []` is a usage-only trailer (recover via
    /// `synthesized_message()`) vs. an actual empty response (retry).
    const nlohmann::json& accumulated_tool_calls() const noexcept override {
        return accumulated_tool_calls_;
    }

private:
    void parse_line(const std::string& line);

    std::weak_ptr<hive::AgentSink> weak_sink_;
    std::string buffer_;
    std::string accumulated_text_;           /// Complete assistant text from deltas.
    /// §ThinkTagFilter: strips inline <think>...</think> blocks models
    /// embed in `content` (qwen3, MiniMax without honored reasoning_split,
    /// deepseek-style). Stream-safe across chunk boundaries; flushed in
    /// finish(). Reasoning sent as the `reasoning_content` field never
    /// touches this.
    ThinkTagFilter think_filter_;
    std::string accumulated_role_;           /// "assistant" usually; set from delta.role.
    std::string accumulated_finish_reason_;  /// "stop", "tool_calls", "length" — set once.
    /// §ProviderPlugins Phase 4: thinking text from `reasoning_content`
    /// delta fragments (Kimi/Moonshot). Round-trips into
    /// synthesized_message() because Kimi's tool loop rejects histories
    /// where the assistant turn lost its reasoning_content.
    std::string accumulated_reasoning_;
    nlohmann::json accumulated_tool_calls_ = nlohmann::json::array();  /// Rebuilt by tool_calls streaming.
    nlohmann::json final_event_;             /// Last SSE event with usage/finish_reason.
};

// ============================================================================
// §BUG-001 (2026-07-17): gate helper, exposed for unit tests.
//
// The streaming response pipeline in LLMClient.cpp has a gate that decides
// whether a final SSE chunk with `choices: []` is a "usage-only trailer"
// (the server is sending just token-counts after a successful stream) or
// an actual empty response (which should trigger an API retry).
//
// The original gate keyed on `response_json["usage"]` being non-null. That
// broke for MiniMax, which sends `"usage": null` on the final chunk. The
// fix is to key on what the SSE parser already accumulated: if it built up
// a complete message (text OR tool_calls), the trailer is recoverable via
// `synthesized_message()` regardless of what `usage` contains.
//
// This helper centralizes the new logic so unit tests can verify the
// truth table directly.
// ============================================================================
// §ProviderPlugins: keyed on the ProviderStreamParser interface so any
// provider plugin's parser works with the gate.
inline bool is_usage_only_chunk(
    bool stream_enabled,
    const nlohmann::json& response,
    const hive::ProviderStreamParser& parser) noexcept
{
    if (!stream_enabled) return false;
    if (response.contains("choices") && !response["choices"].empty()) {
        return false;  // not a no-choices chunk
    }
    return parser.had_text_deltas()
        || !parser.accumulated_tool_calls().empty();
}
