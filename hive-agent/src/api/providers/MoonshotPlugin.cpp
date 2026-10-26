// SPDX-License-Identifier: BSL-1.0
// src/api/providers/MoonshotPlugin.cpp

#include "api/providers/MoonshotPlugin.h"

#include <algorithm>
#include <cctype>

namespace hive {
namespace {

// Static Kimi model table, from the platform model list
// (https://platform.moonshot.ai/docs/models, 2026-07):
//   kimi-k3.1-pro(-highspeed)  1M context, temperature fixed at 1.0
//   kimi-k3                    1M context, temperature fixed at 1.0
//   kimi-k2.7-code(-highspeed) 256K context, temperature fixed at 1.0
//   kimi-k2.6 / kimi-k2.5      256K context, thinking + non-thinking
//   moonshot-v1-*              8K / 32K / 128K context
// The deprecated kimi-k2 preview/thinking series is deliberately absent.
//
// The Kimi Code subscription endpoint (https://api.kimi.com/coding/v1,
// https://www.kimi.com/code/docs/en/) uses its own model IDs — same
// underlying models, different names:
//   k3 / k3-256k                 Kimi K3 (1M / 256K context)
//   kimi-for-coding(-highspeed)  Kimi K2.7 Code (256K context)
// NOTE: the platform documents the "temperature fixed at 1.0" rule for
// the kimi-k2.7-code IDs; the Kimi Code endpoint enforces the same rule
// for kimi-for-coding (confirmed against the live server 2026-07-31), so
// clamp_temperature() covers both.
const ProviderModelInfo kModels[] = {
    {"kimi-k3.1-pro",             1000000, 1.0, true,  true},
    {"kimi-k3.1-pro-highspeed",   1000000, 1.0, true,  true},
    {"k3.1-pro",                  1000000, 1.0, true,  true},
    {"k3.1-pro-highspeed",        1000000, 1.0, true,  true},
    {"kimi-k3",                     1000000, 1.0, true,  true},
    {"k3",                          1000000, 1.0, true,  true},
    {"k3-256k",                      262144, 1.0, true,  true},
    {"kimi-k2.7-code",               262144, 1.0, true,  true},
    {"kimi-k2.7-code-highspeed",     262144, 1.0, true,  true},
    {"kimi-for-coding",              262144, 1.0, true,  true},
    {"kimi-for-coding-highspeed",    262144, 1.0, true,  true},
    {"kimi-k2.6",                    262144, 1.0, true,  true},
    {"kimi-k2.5",                    262144, 0.6, true,  true},
    {"moonshot-v1-8k",                 8192, 0.3, false, false},
    {"moonshot-v1-32k",               32768, 0.3, false, false},
    {"moonshot-v1-128k",             131072, 0.3, false, false},
    {"moonshot-v1-8k-vision-preview",  8192, 0.3, true,  false},
    {"moonshot-v1-32k-vision-preview", 32768, 0.3, true,  false},
    {"moonshot-v1-128k-vision-preview",131072, 0.3, true,  false},
};

bool iequals(std::string_view a, std::string_view b) {
    return a.size() == b.size() &&
           std::equal(a.begin(), a.end(), b.begin(),
                      [](char x, char y) {
                          return std::tolower(static_cast<unsigned char>(x)) ==
                                 std::tolower(static_cast<unsigned char>(y));
                      });
}

bool icontains(std::string_view haystack, std::string_view needle) {
    if (needle.size() > haystack.size()) return false;
    return std::search(haystack.begin(), haystack.end(),
                       needle.begin(), needle.end(),
                       [](char x, char y) {
                           return std::tolower(static_cast<unsigned char>(x)) ==
                                  std::tolower(static_cast<unsigned char>(y));
                       }) != haystack.end();
}

bool istarts_with(std::string_view s, std::string_view prefix) {
    return s.size() >= prefix.size() && iequals(s.substr(0, prefix.size()), prefix);
}

bool is_k3_family(std::string_view id) {
    return istarts_with(id, "kimi-k3") || iequals(id, "k3") ||
           istarts_with(id, "k3-") || istarts_with(id, "k3.1-");
}

} // namespace

bool MoonshotPlugin::matches(std::string_view host) const {
    // Covers api.moonshot.ai, api.moonshot.cn, and the Kimi Code
    // subscription endpoint api.kimi.com.
    return icontains(host, "moonshot") || icontains(host, "kimi");
}

double MoonshotPlugin::clamp_temperature(std::string_view model_id, double t) const {
    // kimi-k2.7-code(-highspeed) uses a fixed temperature of 1.0 — any
    // other value is a hard API error (platform docs, "Parameters
    // Differences in Request Body"). The Kimi Code endpoint's
    // kimi-for-coding IDs are the same K2.7 Code model and enforce the
    // same rule; its k3/k3-256k IDs enforce it too (both confirmed
    // empirically against the live server 2026-07-31: "invalid
    // temperature: only 1 is allowed for this model"). Scoped to the
    // K3/K3.1 platform IDs too (official Model Parameter Reference,
    // 2026-10: K3.1 has the same fixed-1.0 constraint as K3).
    if (icontains(model_id, "k2.7-code") ||
        is_k3_family(model_id) ||
        icontains(model_id, "kimi-for-coding")) {
        return 1.0;
    }
    return t;
}

void MoonshotPlugin::mutate_chat_payload(nlohmann::json& payload,
                                         const ProviderChatContext& ctx) const {
    // Generic shaping first (stream_options.include_usage when streaming).
    OpenAiCompatiblePlugin::mutate_chat_payload(payload, ctx);

    // Server-side prompt caching: a stable per-agent key lets repeated
    // system prompts/tools hit Kimi's context cache. Empty key = opt out.
    if (!ctx.cache_key.empty()) {
        payload["prompt_cache_key"] = ctx.cache_key;
    }

    // §ThinkingLevel (2026-08-01): host-selected thinking level. Mapping
    // (https://platform.kimi.ai/docs/guide/use-thinking-models):
    //   - K3 ids: reasoning effort low/high/max. The Kimi Code
    //     subscription endpoint (api.kimi.com/coding/v1) takes it as
    //     thinking.effort (empirically verified, oh-my-pi #5893); the
    //     platform endpoint takes top-level reasoning_effort and must NOT
    //     receive a `thinking` object. None is ignored — K3 always thinks.
    //   - kimi-k2.5 / kimi-k2.6: on/off only (thinking.type). Effort
    //     levels are ignored.
    //   - kimi-k2.7-code / kimi-for-coding: always thinks, no control —
    //     never send anything (passing disabled is a hard error).
    if (ctx.thinking_level != ThinkingLevel::Default) {
        const std::string& id = ctx.model_id;
        const bool is_k3 = is_k3_family(id);
        const bool is_k25_k26 = icontains(id, "k2.5") || icontains(id, "k2.6");
        if (is_k3 &&
            (ctx.thinking_level == ThinkingLevel::Low ||
             ctx.thinking_level == ThinkingLevel::High ||
             ctx.thinking_level == ThinkingLevel::Max)) {
            std::string effort = to_string(ctx.thinking_level);
            for (auto& c : effort) {
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            }
            if (icontains(ctx.base_url, "kimi.com")) {
                payload["thinking"] = {{"type", "enabled"}, {"effort", effort}};
            } else {
                payload["reasoning_effort"] = effort;
            }
        } else if (is_k25_k26 && ctx.thinking_level == ThinkingLevel::None) {
            payload["thinking"] = {{"type", "disabled"}};
        }
    }
}

std::vector<ThinkingLevel>
MoonshotPlugin::supported_thinking_levels(std::string_view model_id) const {
    if (is_k3_family(model_id)) {
        return {ThinkingLevel::Low, ThinkingLevel::High, ThinkingLevel::Max};
    }
    if (icontains(model_id, "k2.5") || icontains(model_id, "k2.6")) {
        return {ThinkingLevel::None};
    }
    return {};
}

std::vector<ProviderModelInfo> MoonshotPlugin::known_models() const {
    return {std::begin(kModels), std::end(kModels)};
}

int64_t MoonshotPlugin::context_size_for(std::string_view model_id) const {
    for (const auto& m : kModels) {
        if (iequals(m.model_id, model_id)) return m.context_size;
    }
    return 0;
}

} // namespace hive
