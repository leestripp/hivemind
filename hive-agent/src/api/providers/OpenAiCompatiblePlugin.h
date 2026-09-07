// SPDX-License-Identifier: BSL-1.0
// src/api/providers/OpenAiCompatiblePlugin.h
//
// Default provider plugin — implements every ProviderPlugin hook with
// the library's classic generic OpenAI-compatible behavior. Used for
// any provider row whose `plugin` column names no registered plugin.
//
// Internal header — not part of the public the engine API.

#pragma once

#include "api/ProviderPlugin.h"

namespace hive {

class OpenAiCompatiblePlugin : public ProviderPlugin {
public:
    std::string name() const override { return "openai-compatible"; }
    std::string default_base_url() const override { return "https://api.openai.com"; }

    void mutate_chat_payload(nlohmann::json& payload,
                             const ProviderChatContext& ctx) const override;

    std::unique_ptr<ProviderStreamParser>
        create_stream_parser(std::weak_ptr<AgentSink> sink) const override;

    ModelListParseResult
        parse_models_response(const nlohmann::json& body) const override;

    ErrorKind classify_api_error_type(std::string_view error_type) const override;
};

} // namespace hive
