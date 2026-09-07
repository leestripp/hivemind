// SPDX-License-Identifier: BSL-1.0
// src/api/providers/LmStudioPlugin.h
//
// Provider plugin for LM Studio (and other local OpenAI-compatible
// servers) at http://localhost:1234.
//
// Local servers differ from hosted providers:
//   - No API key is required (requires_auth() = false).
//   - Model ids are whatever the user loaded — there is no meaningful
//     static model table, so known_models() stays empty and discovery
//     (/v1/models) is the source of truth.
//   - The payload must stay minimal: LM Studio builds reject unknown
//     request fields (reasoning_split, prompt_cache_key, thinking, ...)
//     depending on version, so mutate_chat_payload() strips those
//     provider extensions. The one exception is
//     stream_options.include_usage, which IS sent in streaming mode —
//     LM Studio accepts it and token accounting (and therefore the
//     auto-compression trigger) depends on it.
//
// Internal header — not part of the public the engine API.

#pragma once

#include "api/providers/OpenAiCompatiblePlugin.h"

namespace hive {

class LmStudioPlugin : public OpenAiCompatiblePlugin {
public:
    std::string name() const override { return "lmstudio"; }
    std::string default_base_url() const override { return "http://localhost:1234"; }

    bool matches(std::string_view host) const override;

    bool requires_auth() const override { return false; }

    void mutate_chat_payload(nlohmann::json& payload,
                             const ProviderChatContext& ctx) const override;
};

} // namespace hive
