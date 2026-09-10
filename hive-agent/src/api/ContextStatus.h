// SPDX-License-Identifier: BSL-1.0
// src/api/ContextStatus.h
//
// §ContextCache (2026-07-31): prompt-cache helpers, factored out of
// LLMClient so unit tests can exercise them directly (same pattern as
// MiniMaxSseParser.h — private header, exposed for unit tests).
//
// Two pieces:
//   1. extract_cached_tokens() — folds the two provider usage shapes
//      (Moonshot top-level `cached_tokens`, MiniMax/OpenAI nested
//      `prompt_tokens_details.cached_tokens`) into one counter.
//   2. upsert_context_status_message() — carries the per-iteration
//      Context Status block as a transient TRAILING system message
//      instead of embedding it in the system prompt.
//
// Why a trailing message: MiniMax and Kimi both match server-side
// prompt caches on a stable request prefix (tools → system → messages).
// A stats block that changes every iteration — previously embedded near
// the TOP of the system message — invalidated everything after the
// first differing byte: the rest of the system prompt AND the whole
// conversation history. As a trailing message, only the stats message
// itself (and messages newer than the last request) ever miss; the
// entire static prefix stays cache-eligible.
#pragma once

#include <cstring>
#include <string>

#include <nlohmann/json.hpp>

namespace hive::context_status {

/// Marker prepended to the transient status message content so the next
/// iteration can find and replace exactly that message.
inline constexpr const char* kBeginMarker = "<!-- hive-agent-context-status -->\n";

/// Terminator written at the end of the stats block (kept from the §B8
/// fix) — also used to recognise legacy embedded blocks.
inline constexpr const char* kEndSentinel = "<!-- /hive-agent-context-status -->\n";

/// Fold the provider's cache-hit count out of a `usage` object.
///   - Moonshot/Kimi: top-level "cached_tokens"
///     (platform.kimi.ai/docs/api/chat)
///   - MiniMax + OpenAI: nested "prompt_tokens_details.cached_tokens"
/// Returns 0 when the provider doesn't report cache hits.
inline int extract_cached_tokens(const nlohmann::json& usage) {
    if (!usage.is_object()) return 0;
    int cached = usage.value("cached_tokens", 0);
    if (cached == 0 && usage.contains("prompt_tokens_details") &&
        usage["prompt_tokens_details"].is_object()) {
        cached = usage["prompt_tokens_details"].value("cached_tokens", 0);
    }
    return cached;
}

/// Strip a legacy embedded `#/## Context Status` block (pre-§ContextCache
/// sessions persisted it inside the system message) from `content`.
/// Erases from the heading to the end sentinel; falls back to the next
/// same-level heading / blank line when the sentinel is missing.
inline void strip_embedded_status_block(std::string& content) {
    // Search the H2 form first: "# Context Status" is a substring of
    // "## Context Status", so an H1-first search would match one byte
    // into the H2 heading and leave a stray '#' behind on erase.
    size_t context_start = content.find("## Context Status");
    bool is_h2 = context_start != std::string::npos;
    if (!is_h2) {
        context_start = content.find("# Context Status");
    }
    if (context_start == std::string::npos) return;

    size_t sentinel_pos = content.find(kEndSentinel, context_start);
    size_t context_end;
    if (sentinel_pos != std::string::npos) {
        context_end = sentinel_pos + std::strlen(kEndSentinel);
    } else {
        std::string end_marker = is_h2 ? "\n## " : "\n# ";
        size_t next_heading = content.find(end_marker, context_start + 1);
        if (next_heading != std::string::npos) {
            context_end = next_heading;
        } else {
            size_t blank = content.find("\n\n", context_start);
            context_end = (blank != std::string::npos) ? blank + 2
                                                       : content.size();
        }
    }
    content.erase(context_start, context_end - context_start);
}

/// Insert or replace the per-iteration Context Status message as the
/// LAST message of the wire array.
///
///   - Any legacy block embedded in the system message is stripped
///     (keeps the persistent system prompt static across iterations).
///   - The previous iteration's status message (anywhere in the array —
///     assistant/user messages are appended after it during a turn) is
///     removed, so exactly one status message exists at any time.
///   - The fresh status message is appended last, leaving the whole
///     preceding array byte-identical to the previous request's prefix.
///
/// `stats_block` is the fully-formatted status text (without the begin
/// marker — it is added here).
inline void upsert_context_status_message(nlohmann::json& messages,
                                          const std::string& stats_block) {
    // 1. Legacy cleanup: static system prompt must stay static.
    if (!messages.empty() && messages[0].value("role", "") == "system") {
        std::string content = messages[0].value("content", "");
        strip_embedded_status_block(content);
        messages[0]["content"] = content;
    }

    // 2. Drop the previous transient status message(s).
    for (auto it = messages.begin(); it != messages.end();) {
        if (it->value("role", "") == "system" &&
            it->value("content", "").find(kBeginMarker) != std::string::npos) {
            it = messages.erase(it);
        } else {
            ++it;
        }
    }

    // 3. Append the fresh status message as the last message.
    messages.push_back({
        {"role", "system"},
        {"content", std::string(kBeginMarker) + stats_block},
    });
}

} // namespace hive::context_status
