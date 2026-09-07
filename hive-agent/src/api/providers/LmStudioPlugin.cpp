// SPDX-License-Identifier: BSL-1.0
// src/api/providers/LmStudioPlugin.cpp

#include "api/providers/LmStudioPlugin.h"

namespace hive {

bool LmStudioPlugin::matches(std::string_view host) const {
    // Any loopback OpenAI-compatible server. LM Studio's default is
    // http://localhost:1234; matching all loopback hosts also covers
    // llama.cpp/ollama-style servers on other ports.
    return host.find("localhost") != std::string_view::npos ||
           host.find("127.0.0.1") != std::string_view::npos;
}

void LmStudioPlugin::mutate_chat_payload(nlohmann::json& payload,
                                         const ProviderChatContext& ctx) const {
    // Deliberately does NOT call the base hook: provider-extension fields
    // (reasoning_split, prompt_cache_key, thinking, reasoning_effort) are
    // stripped so the request stays within the plain OpenAI
    // chat-completions shape local servers accept.
    payload.erase("reasoning_split");
    payload.erase("prompt_cache_key");
    payload.erase("thinking");
    payload.erase("reasoning_effort");

    // §fix (2026-08-01): stream_options.include_usage IS sent. LM Studio
    // accepts it, and without a usage payload token accounting reads 0 —
    // which silently disables the 90%-of-context auto-compression trigger
    // for every LM Studio turn. Previously this was stripped along with
    // the genuine provider extensions above.
    if (ctx.stream) {
        payload["stream_options"] = {{"include_usage", true}};
    } else {
        payload.erase("stream_options");
    }
}

} // namespace hive
