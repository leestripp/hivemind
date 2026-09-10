// SPDX-License-Identifier: BSL-1.0
// tests/test_thinking_levels.cpp
//
// §ThinkingLevel (2026-08-01): user-selectable thinking/reasoning levels.
//   T1. ThinkingLevel string helpers (to_string / from_string).
//   T2. Moonshot K3 ids: platform host -> top-level reasoning_effort;
//       Kimi Code host (api.kimi.com) -> thinking.effort; None ignored.
//   T3. Moonshot kimi-k2.5/k2.6: None -> thinking.type=disabled; effort
//       levels ignored. k2.7-code / kimi-for-coding: never touched.
//   T4. MiniMax: M3 + None -> thinking.type=disabled; M2.x untouched.
//   T5. LmStudio strips thinking/reasoning_effort.
//   T6. supported_thinking_levels() capability matrix; default plugin
//       reports none.
//   T7. Runtime toggle: LLMClient::set_thinking_level() applies without
//       rebuilding the client.

#include <filesystem>
#include <iostream>
#include <memory>
#include <string>

#include <nlohmann/json.hpp>

#include "api/ProviderPlugin.h"
#include "api/ProviderRegistry.h"
#include "types/ThinkingLevel.h"

#include "api/LLMClient.h"

using json = nlohmann::json;
using hive::ThinkingLevel;

namespace {

int failures = 0;
#define EXPECT(cond) do { \
    if (!(cond)) { \
        std::cerr << "FAIL " << __FILE__ << ":" << __LINE__ \
                  << " EXPECT(" #cond ")\n"; \
        ++failures; \
    } \
} while (0)

hive::ProviderChatContext make_ctx(const std::string& model_id,
                                       const std::string& base_url,
                                       ThinkingLevel level) {
    hive::ProviderChatContext ctx;
    ctx.model_id = model_id;
    ctx.base_url = base_url;
    ctx.thinking_level = level;
    return ctx;
}

void test_t1_string_helpers() {
    EXPECT(std::string(hive::to_string(ThinkingLevel::Default)) == "Default");
    EXPECT(std::string(hive::to_string(ThinkingLevel::None))    == "None");
    EXPECT(std::string(hive::to_string(ThinkingLevel::Low))     == "Low");
    EXPECT(std::string(hive::to_string(ThinkingLevel::High))    == "High");
    EXPECT(std::string(hive::to_string(ThinkingLevel::Max))     == "Max");

    EXPECT(hive::thinking_level_from_string("none") == ThinkingLevel::None);
    EXPECT(hive::thinking_level_from_string("LOW")  == ThinkingLevel::Low);
    EXPECT(hive::thinking_level_from_string("High") == ThinkingLevel::High);
    EXPECT(hive::thinking_level_from_string("max")  == ThinkingLevel::Max);
    EXPECT(hive::thinking_level_from_string("bogus") == ThinkingLevel::Default);
    EXPECT(hive::thinking_level_from_string("")      == ThinkingLevel::Default);
    std::cout << "[ok] T1: ThinkingLevel string helpers\n";
}

void test_t2_moonshot_k3() {
    auto p = hive::ProviderRegistry::plugin_for_name("moonshot");
    EXPECT(p != nullptr);

    for (const char* id : {"kimi-k3", "k3", "k3-256k"}) {
        // Platform endpoint -> top-level reasoning_effort, no thinking object.
        json payload = {{"model", id}, {"messages", json::array()}};
        p->mutate_chat_payload(payload,
            make_ctx(id, "https://api.moonshot.ai", ThinkingLevel::Low));
        EXPECT(payload.value("reasoning_effort", "") == "low");
        EXPECT(!payload.contains("thinking"));

        // Kimi Code subscription endpoint -> thinking.effort, no top-level field.
        json payload2 = {{"model", id}, {"messages", json::array()}};
        p->mutate_chat_payload(payload2,
            make_ctx(id, "https://api.kimi.com/coding/v1", ThinkingLevel::High));
        EXPECT(payload2.contains("thinking"));
        EXPECT(payload2["thinking"].value("type", "") == "enabled");
        EXPECT(payload2["thinking"].value("effort", "") == "high");
        EXPECT(!payload2.contains("reasoning_effort"));

        // Max maps to "max".
        json payload3 = {{"model", id}, {"messages", json::array()}};
        p->mutate_chat_payload(payload3,
            make_ctx(id, "https://api.moonshot.ai", ThinkingLevel::Max));
        EXPECT(payload3.value("reasoning_effort", "") == "max");

        // None is ignored — K3 always thinks, and `thinking` must not be sent.
        json payload4 = {{"model", id}, {"messages", json::array()}};
        p->mutate_chat_payload(payload4,
            make_ctx(id, "https://api.moonshot.ai", ThinkingLevel::None));
        EXPECT(!payload4.contains("thinking"));
        EXPECT(!payload4.contains("reasoning_effort"));

        // Default sends nothing either.
        json payload5 = {{"model", id}, {"messages", json::array()}};
        p->mutate_chat_payload(payload5,
            make_ctx(id, "https://api.moonshot.ai", ThinkingLevel::Default));
        EXPECT(!payload5.contains("thinking"));
        EXPECT(!payload5.contains("reasoning_effort"));
    }
    std::cout << "[ok] T2: Moonshot K3 effort mapping (platform + coding hosts)\n";
}

void test_t3_moonshot_k2() {
    auto p = hive::ProviderRegistry::plugin_for_name("moonshot");
    EXPECT(p != nullptr);

    for (const char* id : {"kimi-k2.5", "kimi-k2.6"}) {
        json payload = {{"model", id}, {"messages", json::array()}};
        p->mutate_chat_payload(payload,
            make_ctx(id, "https://api.moonshot.ai", ThinkingLevel::None));
        EXPECT(payload.contains("thinking"));
        EXPECT(payload["thinking"].value("type", "") == "disabled");
        EXPECT(!payload["thinking"].contains("effort"));

        // Effort levels are not supported on k2.5/k2.6 — ignored.
        json payload2 = {{"model", id}, {"messages", json::array()}};
        p->mutate_chat_payload(payload2,
            make_ctx(id, "https://api.moonshot.ai", ThinkingLevel::Low));
        EXPECT(!payload2.contains("thinking"));
        EXPECT(!payload2.contains("reasoning_effort"));
    }

    // k2.7-code / kimi-for-coding: always thinks, NO control — any level
    // must leave the payload untouched (passing disabled is a hard error).
    for (const char* id : {"kimi-k2.7-code", "kimi-k2.7-code-highspeed",
                           "kimi-for-coding", "kimi-for-coding-highspeed"}) {
        for (ThinkingLevel lvl : {ThinkingLevel::None, ThinkingLevel::Low,
                                  ThinkingLevel::High, ThinkingLevel::Max}) {
            json payload = {{"model", id}, {"messages", json::array()}};
            p->mutate_chat_payload(payload,
                make_ctx(id, "https://api.kimi.com/coding/v1", lvl));
            EXPECT(!payload.contains("thinking"));
            EXPECT(!payload.contains("reasoning_effort"));
        }
    }
    std::cout << "[ok] T3: Moonshot k2.x on/off + no-control models\n";
}

void test_t4_minimax() {
    auto p = hive::ProviderRegistry::plugin_for_name("minimax");
    EXPECT(p != nullptr);

    // M3 + None -> thinking disabled (reasoning_split still applied).
    json payload = {{"model", "MiniMax-M3"}, {"messages", json::array()}};
    p->mutate_chat_payload(payload,
        make_ctx("MiniMax-M3", "https://api.minimax.io", ThinkingLevel::None));
    EXPECT(payload.contains("thinking"));
    EXPECT(payload["thinking"].value("type", "") == "disabled");
    EXPECT(payload.value("reasoning_split", false) == true);

    // M3 + effort level: no such thing on MiniMax — ignored.
    json payload2 = {{"model", "MiniMax-M3"}, {"messages", json::array()}};
    p->mutate_chat_payload(payload2,
        make_ctx("MiniMax-M3", "https://api.minimax.io", ThinkingLevel::Low));
    EXPECT(!payload2.contains("thinking"));

    // M2.x: thinking cannot be disabled — send nothing.
    json payload3 = {{"model", "MiniMax-M2.7"}, {"messages", json::array()}};
    p->mutate_chat_payload(payload3,
        make_ctx("MiniMax-M2.7", "https://api.minimax.io", ThinkingLevel::None));
    EXPECT(!payload3.contains("thinking"));
    std::cout << "[ok] T4: MiniMax M3 on/off, M2.x untouched\n";
}

void test_t5_lmstudio_strips() {
    auto p = hive::ProviderRegistry::plugin_for_name("lmstudio");
    EXPECT(p != nullptr);

    json payload = {{"model", "local"},
                    {"messages", json::array()},
                    {"thinking", {{"type", "disabled"}}},
                    {"reasoning_effort", "low"},
                    {"stream_options", {{"include_usage", true}}}};
    p->mutate_chat_payload(payload,
        make_ctx("local", "http://localhost:1234", ThinkingLevel::None));
    EXPECT(!payload.contains("thinking"));
    EXPECT(!payload.contains("reasoning_effort"));
    // stream_options is kept in streaming mode (include_usage forced on for
    // token accounting); make_ctx leaves ctx.stream = true.
    EXPECT(payload.contains("stream_options"));
    EXPECT(payload["stream_options"].value("include_usage", false) == true);
    std::cout << "[ok] T5: LmStudio strips thinking fields\n";
}

void test_t6_capability_query() {
    auto moonshot = hive::ProviderRegistry::plugin_for_name("moonshot");
    auto minimax  = hive::ProviderRegistry::plugin_for_name("minimax");
    auto fallback = hive::ProviderRegistry::default_plugin();
    EXPECT(moonshot != nullptr && minimax != nullptr && fallback != nullptr);

    EXPECT(moonshot->supported_thinking_levels("kimi-k3").size() == 3);
    EXPECT(moonshot->supported_thinking_levels("k3-256k").size() == 3);
    EXPECT(moonshot->supported_thinking_levels("kimi-k2.6").size() == 1);
    EXPECT(moonshot->supported_thinking_levels("kimi-k2.6")[0] == ThinkingLevel::None);
    EXPECT(moonshot->supported_thinking_levels("kimi-for-coding").empty());
    EXPECT(moonshot->supported_thinking_levels("moonshot-v1-8k").empty());

    EXPECT(minimax->supported_thinking_levels("MiniMax-M3").size() == 1);
    EXPECT(minimax->supported_thinking_levels("MiniMax-M3")[0] == ThinkingLevel::None);
    EXPECT(minimax->supported_thinking_levels("MiniMax-M2.7").empty());

    EXPECT(fallback->supported_thinking_levels("anything").empty());
    std::cout << "[ok] T6: supported_thinking_levels capability matrix\n";
}

void test_t7_runtime_toggle() {
    // Hosts flip the level mid-session via Agent::set_thinking_level(),
    // which forwards to LLMClient::set_thinking_level(). The payload hook
    // reads config_.thinking_level per request, so no rebuild is needed.
    const auto scratch = std::filesystem::temp_directory_path() /
                         "thinking-levels-t7";
    std::filesystem::create_directories(scratch);

    AppConfig cfg;
    cfg.model            = "kimi-k3";
    cfg.provider         = "moonshot";
    cfg.base_url         = "https://api.moonshot.ai";
    cfg.plugin           = "moonshot";
    cfg.memory_disabled  = true;
    cfg.target_directory = scratch.string();

    LLMClient client(cfg);
    EXPECT(client.thinking_level() == ThinkingLevel::Default);
    client.set_thinking_level(ThinkingLevel::Max);
    EXPECT(client.thinking_level() == ThinkingLevel::Max);
    client.set_thinking_level(ThinkingLevel::None);
    EXPECT(client.thinking_level() == ThinkingLevel::None);

    std::error_code ec;
    std::filesystem::remove_all(scratch, ec);
    std::cout << "[ok] T7: runtime thinking-level toggle\n";
}

} // namespace

int main() {
    test_t1_string_helpers();
    test_t2_moonshot_k3();
    test_t3_moonshot_k2();
    test_t4_minimax();
    test_t5_lmstudio_strips();
    test_t6_capability_query();
    test_t7_runtime_toggle();
    if (failures > 0) {
        std::cerr << failures << " failure(s)\n";
        return 1;
    }
    std::cout << "all thinking-level tests passed\n";
    return 0;
}
