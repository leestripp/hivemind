// SPDX-License-Identifier: BSL-1.0
// src/api/providers/OpenAiCompatiblePlugin.cpp
//
// Default-hook implementations for ProviderPlugin's virtual methods,
// plus the OpenAiCompatiblePlugin's classic behavior. All defaults live
// here (not in the public header) so the interface stays a pure
// contract.

#include "api/providers/OpenAiCompatiblePlugin.h"

#include "api/ErrorMapping.h"     // classic error-type classification
#include "api/MiniMaxSseParser.h" // §ProviderPlugins Phase 1: the classic SSE parser

namespace hive {

// ---------------------------------------------------------------------------
// ProviderPlugin virtual defaults
// ---------------------------------------------------------------------------

bool ProviderPlugin::matches(std::string_view) const { return false; }

std::string ProviderPlugin::chat_endpoint() const { return "/v1/chat/completions"; }
std::string ProviderPlugin::models_endpoint() const { return "/v1/models"; }
bool ProviderPlugin::requires_auth() const { return true; }

std::vector<ProviderModelInfo> ProviderPlugin::known_models() const { return {}; }
int64_t ProviderPlugin::context_size_for(std::string_view) const { return 0; }

void ProviderPlugin::mutate_chat_payload(nlohmann::json&,
                                         const ProviderChatContext&) const {}

double ProviderPlugin::clamp_temperature(std::string_view, double t) const { return t; }

void ProviderPlugin::sanitize_history(nlohmann::json&) const {}

std::unique_ptr<ProviderStreamParser>
ProviderPlugin::create_stream_parser(std::weak_ptr<AgentSink>) const { return nullptr; }

ModelListParseResult
ProviderPlugin::parse_models_response(const nlohmann::json&) const { return {}; }

ErrorKind ProviderPlugin::classify_api_error_type(std::string_view) const {
    return ErrorKind::ApiError;
}

std::vector<ThinkingLevel>
ProviderPlugin::supported_thinking_levels(std::string_view) const { return {}; }

// ---------------------------------------------------------------------------
// OpenAiCompatiblePlugin — classic generic behavior
// ---------------------------------------------------------------------------

void OpenAiCompatiblePlugin::mutate_chat_payload(nlohmann::json& payload,
                                                 const ProviderChatContext& ctx) const {
    // Generic request shaping. The MiniMax-specific quirks
    // (reasoning_split, continuous_usage_stats) moved to MiniMaxPlugin in
    // §ProviderPlugins Phase 3 — behavior for MiniMax is unchanged
    // because its provider row resolves to the MiniMax plugin.
    //
    // §Phase 9.10: request usage stats in streaming mode so token
    // accounting (and therefore compression triggering) works.
    if (ctx.stream) {
        payload["stream_options"] = {
            {"include_usage", true}
        };
    }
}

std::unique_ptr<ProviderStreamParser>
OpenAiCompatiblePlugin::create_stream_parser(std::weak_ptr<AgentSink> sink) const {
    // MiniMaxSseParser is the only SSE parser in the tree and handles the
    // plain OpenAI dialect too (its MiniMax-only extensions are inert
    // unless the server sends them). MiniMaxPlugin returns the same
    // parser explicitly; splitting a lean OpenAI-only parser out is
    // deferred future work.
    return std::make_unique<MiniMaxSseParser>(std::move(sink));
}

ModelListParseResult
OpenAiCompatiblePlugin::parse_models_response(const nlohmann::json& body) const {
    // Classic OpenAI list shape, moved from model_discovery.cpp.
    ModelListParseResult out;
    if (body.value("object", "") != "list") {
        out.error = "response object is not 'list'";
        return out;
    }
    if (!body.contains("data") || !body["data"].is_array()) {
        out.error = "missing or invalid 'data' array";
        return out;
    }

    for (const auto& item : body["data"]) {
        if (!item.is_object() || !item.contains("id")) continue;

        DiscoveredModel model;
        model.id = item.value("id", "");
        model.owned_by = item.value("owned_by", "");
        if (item.contains("created") && item["created"].is_number()) {
            model.created = item["created"].get<std::int64_t>();
        }
        if (item.contains("context_length") && item["context_length"].is_number()) {
            model.context_length = item["context_length"].get<std::int64_t>();
        }
        // Kimi returns separate image/video flags; treat either as vision.
        if (item.contains("supports_image_in") && item["supports_image_in"].is_boolean()) {
            model.supports_vision = model.supports_vision || item["supports_image_in"].get<bool>();
        }
        if (item.contains("supports_video_in") && item["supports_video_in"].is_boolean()) {
            model.supports_vision = model.supports_vision || item["supports_video_in"].get<bool>();
        }
        // Some providers use a generic vision flag.
        if (item.contains("supports_vision") && item["supports_vision"].is_boolean()) {
            model.supports_vision = model.supports_vision || item["supports_vision"].get<bool>();
        }
        if (item.contains("supports_reasoning") && item["supports_reasoning"].is_boolean()) {
            model.supports_reasoning = item["supports_reasoning"].get<bool>();
        }
        out.models.push_back(std::move(model));
    }
    return out;
}

ErrorKind OpenAiCompatiblePlugin::classify_api_error_type(std::string_view error_type) const {
    return error_kind_from_api_type(error_type);
}

} // namespace hive
