// SPDX-License-Identifier: BSL-1.0
// src/api/providers/MiniMaxPlugin.h
//
// Provider plugin for MiniMax (https://api.minimax.io).
//
// Encapsulates the MiniMax quirks that used to be hardcoded in
// LLMClient/SettingsStore:
//   - `reasoning_split: true` on every request (§BUG-MINIMAX-105) so
//     thinking arrives in `reasoning_details` instead of a "(think)"
//     content prefix.
//   - `stream_options.continuous_usage_stats` for per-chunk usage
//     (MiniMax-only extension; without include_usage every chunk's
//     usage is null — §Phase 9.10).
//   - The MiniMax SSE chunk dialect (MiniMaxSseParser: whole-object
//     tool_call chunks, audio_content, usage-only trailer chunks).
//   - Static model table (M3/M2.x context sizes).
//
// Internal header — not part of the public libdaedalus API.

#pragma once

#include "api/providers/OpenAiCompatiblePlugin.h"

namespace hive {

class MiniMaxPlugin : public OpenAiCompatiblePlugin {
public:
    std::string name() const override { return "minimax"; }
    std::string default_base_url() const override { return "https://api.minimax.io"; }

    bool matches(std::string_view host) const override;

    void mutate_chat_payload(nlohmann::json& payload,
                             const ProviderChatContext& ctx) const override;

    std::unique_ptr<ProviderStreamParser>
    create_stream_parser(std::weak_ptr<AgentSink> sink) const override;

    std::vector<ProviderModelInfo> known_models() const override;
    int64_t context_size_for(std::string_view model_id) const override;

    std::vector<ThinkingLevel>
        supported_thinking_levels(std::string_view model_id) const override;
};

} // namespace hive
