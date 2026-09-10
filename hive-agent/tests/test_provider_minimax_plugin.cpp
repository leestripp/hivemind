// SPDX-License-Identifier: BSL-1.0
// tests/test_provider_minimax_plugin.cpp
//
// §ProviderPlugins Phase 3: MiniMaxPlugin behavior.
//   MM1. Resolution: provider "MiniMax" name-matches; api.minimax.io
//        host-matches.
//   MM2. Payload mutation: reasoning_split always on; stream_options with
//        include_usage + continuous_usage_stats only when streaming.
//   MM3. context_size_for: wire ids, settings keys, and aliases.
//   MM4. known_models is non-empty and consistent with context_size_for.
//   MM5. create_stream_parser returns a working MiniMaxSseParser.

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

void test_mm1_resolution() {
    auto by_name = hive::ProviderRegistry::plugin_for_name("MiniMax");
    EXPECT(by_name != nullptr);
    EXPECT(by_name->name() == "minimax");

    auto by_host = hive::ProviderRegistry::plugin_for_host("https://api.minimax.io");
    EXPECT(by_host != nullptr);
    EXPECT(by_host->name() == "minimax");

    auto resolved = hive::ProviderRegistry::resolve("MiniMax", "https://api.minimax.io");
    EXPECT(resolved != nullptr);
    EXPECT(resolved->name() == "minimax");

    EXPECT(by_name->default_base_url() == "https://api.minimax.io");
    std::cout << "[ok] MM1: MiniMax plugin resolution\n";
}

void test_mm2_payload_mutation() {
    auto p = hive::ProviderRegistry::plugin_for_name("minimax");
    EXPECT(p != nullptr);

    hive::ProviderChatContext ctx;
    ctx.model_id = "MiniMax-M2.7";
    ctx.stream = true;
    json payload = {{"model", "MiniMax-M2.7"}, {"messages", json::array()}, {"stream", true}};
    p->mutate_chat_payload(payload, ctx);
    EXPECT(payload.value("reasoning_split", false) == true);
    EXPECT(payload.contains("stream_options"));
    EXPECT(payload["stream_options"].value("include_usage", false) == true);
    EXPECT(payload["stream_options"].value("continuous_usage_stats", false) == true);

    ctx.stream = false;
    json payload2 = {{"model", "MiniMax-M2.7"}, {"messages", json::array()}, {"stream", false}};
    p->mutate_chat_payload(payload2, ctx);
    EXPECT(payload2.value("reasoning_split", false) == true);
    EXPECT(!payload2.contains("stream_options"));
    std::cout << "[ok] MM2: MiniMax payload mutation\n";
}

void test_mm3_context_sizes() {
    auto p = hive::ProviderRegistry::plugin_for_name("minimax");
    EXPECT(p != nullptr);

    EXPECT(p->context_size_for("MiniMax-M3") == 500000);
    EXPECT(p->context_size_for("minimax-m3") == 500000);   // settings key
    EXPECT(p->context_size_for("MiniMax-M2.7") == 200000);
    EXPECT(p->context_size_for("minimax-m2.7") == 200000); // settings key
    EXPECT(p->context_size_for("minimax") == 500000);      // alias
    EXPECT(p->context_size_for("MiniMax-M2") == 200000);
    EXPECT(p->context_size_for("no-such-model") == 0);     // unknown
    std::cout << "[ok] MM3: MiniMax context sizes\n";
}

void test_mm4_known_models() {
    auto p = hive::ProviderRegistry::plugin_for_name("minimax");
    EXPECT(p != nullptr);
    const auto models = p->known_models();
    EXPECT(!models.empty());
    for (const auto& m : models) {
        EXPECT(!m.model_id.empty());
        EXPECT(m.context_size > 0);
        // Every known model must round-trip through context_size_for.
        EXPECT(p->context_size_for(m.model_id) == m.context_size);
    }
    std::cout << "[ok] MM4: MiniMax known_models consistent\n";
}

void test_mm5_stream_parser() {
    auto p = hive::ProviderRegistry::plugin_for_name("minimax");
    EXPECT(p != nullptr);
    auto parser = p->create_stream_parser({});
    EXPECT(parser != nullptr);
    // Feed a plain OpenAI-style chunk; the MiniMax dialect parser handles it.
    parser->on_chunk("data: {\"choices\":[{\"delta\":{\"content\":\"hi\"}}]}\n\n");
    parser->finish();
    EXPECT(parser->had_text_deltas());
    EXPECT(parser->accumulated_tool_calls().empty());
    std::cout << "[ok] MM5: MiniMax stream parser\n";
}

} // namespace

int main() {
    test_mm1_resolution();
    test_mm2_payload_mutation();
    test_mm3_context_sizes();
    test_mm4_known_models();
    test_mm5_stream_parser();
    if (failures > 0) {
        std::cerr << failures << " failure(s)\n";
        return 1;
    }
    std::cout << "all MiniMax plugin tests passed\n";
    return 0;
}
