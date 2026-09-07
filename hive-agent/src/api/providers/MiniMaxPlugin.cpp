// SPDX-License-Identifier: BSL-1.0
// src/api/providers/MiniMaxPlugin.cpp

#include "api/providers/MiniMaxPlugin.h"

#include <algorithm>

#include "api/MiniMaxSseParser.h"

namespace hive {
namespace {

// Static MiniMax model table. Context sizes deliberately match the
// values the library has always used (former builtin_max_context_size
// map in SettingsStore.cpp) — note the platform docs advertise a 1M
// window for MiniMax-M3 while the library's proven working value is
// 500K; raising it is a separate, deliberate change.
const ProviderModelInfo kModels[] = {
    {"MiniMax-M3",             500000, 1.0, true,  true},
    {"MiniMax-M2.7",           200000, 1.0, false, true},
    {"MiniMax-M2.7-highspeed", 200000, 1.0, false, true},
    {"MiniMax-M2.5",           200000, 1.0, false, true},
    {"MiniMax-M2.5-highspeed", 200000, 1.0, false, true},
    {"MiniMax-M2.1",           200000, 1.0, false, true},
    {"MiniMax-M2.1-highspeed", 200000, 1.0, false, true},
    {"MiniMax-M2",             200000, 1.0, false, true},
};

// The store's settings keys are lowercase ("minimax-m2.7") while the
// wire id is "MiniMax-M2.7" — match either, case-insensitively.
bool iequals(std::string_view a, std::string_view b) {
    return a.size() == b.size() &&
           std::equal(a.begin(), a.end(), b.begin(),
                      [](char x, char y) {
                          return std::tolower(static_cast<unsigned char>(x)) ==
                                 std::tolower(static_cast<unsigned char>(y));
                      });
}

} // namespace

bool MiniMaxPlugin::matches(std::string_view host) const {
    return host.find("minimax") != std::string_view::npos;
}

void MiniMaxPlugin::mutate_chat_payload(nlohmann::json& payload,
                                        const ProviderChatContext& ctx) const {
    // MiniMax needs the full classic quirk set — this is where it lives
    // now (moved out of the default plugin in §ProviderPlugins Phase 3):
    //
    // §BUG-MINIMAX-105 (2026-07-17): MiniMax's streaming mode mixes
    // thinking content into the `content` field as a literal prefix like
    // "(think)\nThe user wants me to...". The official fix (per MiniMax's
    // own response on issue #105 in MiniMax-M2) is `reasoning_split: true`,
    // which moves thinking into `reasoning_details`.
    //   https://github.com/MiniMax-AI/MiniMax-M2/issues/105
    //   https://platform.minimax.io/docs/api-reference/text-openai-api
    payload["reasoning_split"] = true;

    // §Phase 9.10: without `stream_options.include_usage`, every MiniMax
    // chunk carries "usage": null and token accounting (hence compression
    // triggering) breaks. `continuous_usage_stats` is the MiniMax-only
    // extension that emits usage on every chunk for live token counts.
    if (ctx.stream) {
        payload["stream_options"] = {
            {"include_usage", true},
            {"continuous_usage_stats", true}
        };
    }

    // §ThinkingLevel (2026-08-01): thinking on/off exists only on
    // MiniMax-M3 (`thinking.type` = "disabled"|"adaptive"; omitted = on).
    // M2.x cannot disable thinking ("disabled" is accepted but ignored),
    // and no effort levels exist on any MiniMax model — so the only
    // mapping is M3 + None. Everything else sends nothing.
    // (https://platform.minimax.io/docs/api-reference/text-openai-api)
    if (ctx.thinking_level == ThinkingLevel::None &&
        iequals(ctx.model_id, "MiniMax-M3")) {
        payload["thinking"] = {{"type", "disabled"}};
    }
}

std::vector<ThinkingLevel>
MiniMaxPlugin::supported_thinking_levels(std::string_view model_id) const {
    if (iequals(model_id, "MiniMax-M3")) {
        return {ThinkingLevel::None};
    }
    return {};
}

std::unique_ptr<ProviderStreamParser>
MiniMaxPlugin::create_stream_parser(std::weak_ptr<AgentSink> sink) const {
    // The MiniMax SSE dialect parser (whole-object tool_call chunks,
    // audio_content, usage-only trailer chunks). Implementation lives in
    // LLMClient.cpp.
    return std::make_unique<MiniMaxSseParser>(std::move(sink));
}

std::vector<ProviderModelInfo> MiniMaxPlugin::known_models() const {
    return {std::begin(kModels), std::end(kModels)};
}
int64_t MiniMaxPlugin::context_size_for(std::string_view model_id) const {
    // Aliases first ("minimax" -> M3, "minimax-m2" -> M2), matching the
    // legacy SettingsStore map.
    if (iequals(model_id, "minimax")) return 500000;
    for (const auto& m : kModels) {
        if (iequals(m.model_id, model_id)) return m.context_size;
    }
    return 0;
}

} // namespace hive
