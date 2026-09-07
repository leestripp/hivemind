// SPDX-License-Identifier: BSL-1.0
// src/api/ImageRefs.h
//
// §READ-IMAGE-CORE follow-up (2026-08-10): Markdown image-reference
// extraction, factored into a private header so unit tests can exercise
// it directly (same pattern as ContextStatus.h / MiniMaxSseParser.h).
//
// Hosts attach images to a user message with standard Markdown syntax:
//     Check this screenshot ![ui](shots/login.png) for the error.
// LLMClient::run_one_turn() pulls every ref out of the text and queues
// it for payload-only image_url injection; the stored history keeps the
// cheap Markdown text only.
#pragma once

#include <string>
#include <vector>

namespace hive::image_refs {

/// Extract every image reference (the `(ref)` part of `![alt](ref)`)
/// from `text`, in order of appearance. Refs may be local paths, http(s)
/// URLs, or data URIs. Malformed / unterminated syntax is ignored.
inline std::vector<std::string> extract(const std::string& text) {
    std::vector<std::string> refs;
    size_t pos = 0;
    while ((pos = text.find("![", pos)) != std::string::npos) {
        const size_t alt_end = text.find("](", pos + 2);
        if (alt_end == std::string::npos) break;
        const size_t close = text.find(')', alt_end + 2);
        if (close == std::string::npos) break;
        std::string ref = text.substr(alt_end + 2, close - (alt_end + 2));
        if (!ref.empty()) refs.push_back(std::move(ref));
        pos = close + 1;
    }
    return refs;
}

} // namespace hive::image_refs
