// SPDX-License-Identifier: BSL-1.0
// tests/test_provider_lmstudio_plugin.cpp
//
// §ProviderPlugins Phase 5: LmStudioPlugin behavior.
//   LS1. Resolution: "lmstudio" name; localhost / 127.0.0.1 hosts.
//   LS2. No auth required; default base URL is http://localhost:1234.
//   LS3. Payload stripping: stream_options / reasoning_split /
//        prompt_cache_key removed, nothing added.
//   LS4. No static model table (discovery is the source of truth).

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

void test_ls1_resolution() {
    auto by_name = hive::ProviderRegistry::plugin_for_name("LMStudio");
    EXPECT(by_name != nullptr);
    EXPECT(by_name->name() == "lmstudio");

    for (const char* host : {"http://localhost:1234",
                             "http://127.0.0.1:1234/v1",
                             "http://localhost:8080"}) {
        auto p = hive::ProviderRegistry::plugin_for_host(host);
        EXPECT(p != nullptr);
        EXPECT(p->name() == "lmstudio");
    }
    std::cout << "[ok] LS1: LM Studio plugin resolution\n";
}

void test_ls2_no_auth() {
    auto p = hive::ProviderRegistry::plugin_for_name("lmstudio");
    EXPECT(p != nullptr);
    EXPECT(p->requires_auth() == false);
    EXPECT(p->default_base_url() == "http://localhost:1234");
    std::cout << "[ok] LS2: LM Studio needs no API key\n";
}

void test_ls3_payload_stripping() {
    auto p = hive::ProviderRegistry::plugin_for_name("lmstudio");
    EXPECT(p != nullptr);

    hive::ProviderChatContext ctx;
    ctx.model_id = "qwen3-8b";
    ctx.cache_key = "agent-42";
    ctx.stream = true;
    json payload = {
        {"model", "qwen3-8b"},
        {"messages", json::array()},
        {"stream", true},
        // Simulate fields other layers might have set.
        {"stream_options", {{"include_usage", true}}},
        {"reasoning_split", true},
        {"prompt_cache_key", "agent-42"},
    };
    p->mutate_chat_payload(payload, ctx);
    // Streaming: stream_options.include_usage is forced ON so token
    // accounting / auto-compression work against LM Studio.
    EXPECT(payload.contains("stream_options"));
    EXPECT(payload["stream_options"].value("include_usage", false) == true);
    EXPECT(!payload.contains("reasoning_split"));
    EXPECT(!payload.contains("prompt_cache_key"));
    // Core fields untouched.
    EXPECT(payload.value("model", "") == "qwen3-8b");
    EXPECT(payload.value("stream", false) == true);
    EXPECT(payload.size() == 4);

    // Non-streaming: stream_options is stripped entirely.
    json payload2 = {{"model", "qwen3-8b"},
                     {"messages", json::array()},
                     {"stream_options", {{"include_usage", true}}}};
    ctx.stream = false;
    p->mutate_chat_payload(payload2, ctx);
    EXPECT(!payload2.contains("stream_options"));
    std::cout << "[ok] LS3: LM Studio payload stripping\n";
}

void test_ls4_no_static_models() {
    auto p = hive::ProviderRegistry::plugin_for_name("lmstudio");
    EXPECT(p != nullptr);
    EXPECT(p->known_models().empty());
    EXPECT(p->context_size_for("anything-the-user-loaded") == 0);
    std::cout << "[ok] LS4: LM Studio has no static model table\n";
}

} // namespace

int main() {
    test_ls1_resolution();
    test_ls2_no_auth();
    test_ls3_payload_stripping();
    test_ls4_no_static_models();
    if (failures > 0) {
        std::cerr << failures << " failure(s)\n";
        return 1;
    }
    std::cout << "all LM Studio plugin tests passed\n";
    return 0;
}
