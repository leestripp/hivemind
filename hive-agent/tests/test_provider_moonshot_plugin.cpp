// SPDX-License-Identifier: BSL-1.0
// tests/test_provider_moonshot_plugin.cpp
//
// §ProviderPlugins Phase 4: MoonshotPlugin behavior.
//   MS1. Resolution: "moonshot" name; api.moonshot.ai/.cn and kimi hosts.
//   MS2. clamp_temperature: kimi-k2.7-code forces 1.0, others pass through.
//   MS3. Payload mutation: generic stream_options + prompt_cache_key;
//        no MiniMax quirks leak in.
//   MS4. context_size_for: kimi-k3 / k2.x / moonshot-v1 sizes.
//   MS5. reasoning_content round-trips through the stream parser into
//        the synthesized assistant message (Kimi tool-loop requirement).

#include <iostream>
#include <memory>
#include <string>

#include <nlohmann/json.hpp>

#include "api/ProviderPlugin.h"
#include "api/ProviderRegistry.h"

using json = nlohmann::json;

namespace {

int failures = 0;
#define EXPECT(cond) do { \
    if (!(cond)) { \
        std::cerr << "FAIL " << __FILE__ << ":" << __LINE__ \
                  << " EXPECT(" #cond ")\n"; \
        ++failures; \
    } \
} while (0)

void test_ms1_resolution() {
    auto by_name = hive::ProviderRegistry::plugin_for_name("Moonshot");
    EXPECT(by_name != nullptr);
    EXPECT(by_name->name() == "moonshot");

    for (const char* host : {"https://api.moonshot.ai",
                             "https://api.moonshot.cn",
                             "https://api.kimi.com/coding",
                             "https://api.kimi.example.com"}) {
        auto p = hive::ProviderRegistry::plugin_for_host(host);
        EXPECT(p != nullptr);
        EXPECT(p->name() == "moonshot");
    }
    EXPECT(by_name->default_base_url() == "https://api.moonshot.ai");
    std::cout << "[ok] MS1: Moonshot plugin resolution\n";
}

void test_ms2_temperature_clamp() {
    auto p = hive::ProviderRegistry::plugin_for_name("moonshot");
    EXPECT(p != nullptr);
    // kimi-k2.7-code rejects any temperature other than 1.0.
    EXPECT(p->clamp_temperature("kimi-k2.7-code", 0.0) == 1.0);
    EXPECT(p->clamp_temperature("kimi-k2.7-code", 0.7) == 1.0);
    EXPECT(p->clamp_temperature("kimi-k2.7-code-highspeed", 0.3) == 1.0);
    // Other models pass through unchanged.
    EXPECT(p->clamp_temperature("kimi-k2.6", 0.7) == 0.7);
    EXPECT(p->clamp_temperature("kimi-k3", 0.2) == 1.0);
    EXPECT(p->clamp_temperature("kimi-k3.1-pro", 0.2) == 1.0);
    EXPECT(p->clamp_temperature("kimi-k3.1-pro-highspeed", 0.7) == 1.0);
    EXPECT(p->clamp_temperature("k3.1-pro", 0.2) == 1.0);
    EXPECT(p->clamp_temperature("k3.1-pro-highspeed", 0.7) == 1.0);
    EXPECT(p->clamp_temperature("moonshot-v1-8k", 0.5) == 0.5);
    // Kimi Code endpoint IDs are the same K2.7 Code model — same clamp
    // (the live server rejects other temperatures).
    EXPECT(p->clamp_temperature("kimi-for-coding", 0.0) == 1.0);
    EXPECT(p->clamp_temperature("kimi-for-coding-highspeed", 0.5) == 1.0);
    // Kimi Code and platform K3/K3.1 IDs all enforce temperature=1.0.
    EXPECT(p->clamp_temperature("k3", 0.0) == 1.0);
    EXPECT(p->clamp_temperature("k3-256k", 0.3) == 1.0);
    EXPECT(p->clamp_temperature("kimi-k3", 0.2) == 1.0);
    std::cout << "[ok] MS2: Moonshot temperature clamp\n";
}

void test_ms3_payload_mutation() {
    auto p = hive::ProviderRegistry::plugin_for_name("moonshot");
    EXPECT(p != nullptr);

    hive::ProviderChatContext ctx;
    ctx.model_id = "kimi-k2.6";
    ctx.cache_key = "agent-42";
    ctx.stream = true;
    json payload = {{"model", "kimi-k2.6"}, {"messages", json::array()}, {"stream", true}};
    p->mutate_chat_payload(payload, ctx);
    EXPECT(payload.contains("stream_options"));
    EXPECT(payload["stream_options"].value("include_usage", false) == true);
    EXPECT(payload.value("prompt_cache_key", "") == "agent-42");
    // MiniMax quirks must not leak into Moonshot payloads.
    EXPECT(!payload.contains("reasoning_split"));
    EXPECT(!payload["stream_options"].contains("continuous_usage_stats"));

    // Empty cache key -> no prompt_cache_key field at all.
    ctx.cache_key.clear();
    ctx.stream = false;
    json payload2 = {{"model", "kimi-k2.6"}, {"messages", json::array()}, {"stream", false}};
    p->mutate_chat_payload(payload2, ctx);
    EXPECT(!payload2.contains("prompt_cache_key"));
    EXPECT(!payload2.contains("stream_options"));
    std::cout << "[ok] MS3: Moonshot payload mutation\n";
}

void test_ms4_context_sizes() {
    auto p = hive::ProviderRegistry::plugin_for_name("moonshot");
    EXPECT(p != nullptr);
    EXPECT(p->context_size_for("kimi-k3") == 1000000);
    EXPECT(p->context_size_for("kimi-k3.1-pro") == 1000000);
    EXPECT(p->context_size_for("kimi-k3.1-pro-highspeed") == 1000000);
    EXPECT(p->context_size_for("k3.1-pro") == 1000000);
    EXPECT(p->context_size_for("k3.1-pro-highspeed") == 1000000);
    EXPECT(p->context_size_for("kimi-k2.7-code") == 262144);
    EXPECT(p->context_size_for("kimi-k2.6") == 262144);
    EXPECT(p->context_size_for("Kimi-K2.5") == 262144);   // case-insensitive
    EXPECT(p->context_size_for("moonshot-v1-8k") == 8192);
    EXPECT(p->context_size_for("moonshot-v1-128k") == 131072);
    EXPECT(p->context_size_for("no-such-model") == 0);

    // Kimi Code subscription endpoint model IDs (api.kimi.com/coding/v1).
    EXPECT(p->context_size_for("k3") == 1000000);
    EXPECT(p->context_size_for("k3-256k") == 262144);
    EXPECT(p->context_size_for("kimi-for-coding") == 262144);
    EXPECT(p->context_size_for("kimi-for-coding-highspeed") == 262144);

    const auto models = p->known_models();
    EXPECT(!models.empty());
    for (const auto& m : models) {
        EXPECT(p->context_size_for(m.model_id) == m.context_size);
    }
    std::cout << "[ok] MS4: Moonshot context sizes\n";
}

void test_ms5_reasoning_content_roundtrip() {
    auto p = hive::ProviderRegistry::plugin_for_name("moonshot");
    EXPECT(p != nullptr);
    auto parser = p->create_stream_parser({});
    EXPECT(parser != nullptr);
    // Kimi streams thinking in delta.reasoning_content fragments.
    parser->on_chunk(
        "data: {\"choices\":[{\"delta\":{\"reasoning_content\":\"think \"}}]}\n\n"
        "data: {\"choices\":[{\"delta\":{\"reasoning_content\":\"more\",\"content\":\"answer\"}}]}\n\n");
    parser->finish();
    const auto msg = parser->synthesized_message();
    EXPECT(msg.value("content", "") == "answer");
    EXPECT(msg.contains("reasoning_content"));
    EXPECT(msg.value("reasoning_content", "") == "think more");
    std::cout << "[ok] MS5: reasoning_content round-trip\n";
}

} // namespace

int main() {
    test_ms1_resolution();
    test_ms2_temperature_clamp();
    test_ms3_payload_mutation();
    test_ms4_context_sizes();
    test_ms5_reasoning_content_roundtrip();
    if (failures > 0) {
        std::cerr << failures << " failure(s)\n";
        return 1;
    }
    std::cout << "all Moonshot plugin tests passed\n";
    return 0;
}
