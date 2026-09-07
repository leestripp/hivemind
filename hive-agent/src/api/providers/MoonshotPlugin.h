// SPDX-License-Identifier: BSL-1.0
// src/api/providers/MoonshotPlugin.h
//
// Provider plugin for Moonshot AI's Kimi APIs:
//   - Kimi Platform (pay-as-you-go): https://api.moonshot.ai /
//     https://api.moonshot.cn
//   - Kimi Code (membership subscription): https://api.kimi.com/coding/v1
//     (own model IDs: k3, k3-256k, kimi-for-coding[-highspeed])
//
// Encapsulates the Kimi quirks:
//   - kimi-k2.7-code rejects any temperature other than 1.0 (fixed
//     value server-side) — clamp_temperature() forces it.
//   - `prompt_cache_key` is sent when the agent has a stable name so
//     repeated system prompts hit the server-side context cache.
//   - `reasoning_content` must round-trip in the conversation history
//     during multi-step tool calling (Kimi rejects histories that drop
//     it) — handled by the shared SSE parser accumulating the field
//     into the synthesized assistant message (§ProviderPlugins Phase 4
//     in MiniMaxSseParser/LLMClient.cpp).
//   - Static model table (kimi-k3, kimi-k2.x, moonshot-v1 context sizes).
//
// Internal header — not part of the public libdaedalus API.

#pragma once

#include "api/providers/OpenAiCompatiblePlugin.h"

namespace hive {

class MoonshotPlugin : public OpenAiCompatiblePlugin {
public:
    std::string name() const override { return "moonshot"; }
    std::string default_base_url() const override { return "https://api.moonshot.ai"; }

    bool matches(std::string_view host) const override;

    double clamp_temperature(std::string_view model_id, double t) const override;

    void mutate_chat_payload(nlohmann::json& payload,
                             const ProviderChatContext& ctx) const override;

    std::vector<ProviderModelInfo> known_models() const override;
    int64_t context_size_for(std::string_view model_id) const override;

    std::vector<ThinkingLevel>
        supported_thinking_levels(std::string_view model_id) const override;
};

} // namespace hive
