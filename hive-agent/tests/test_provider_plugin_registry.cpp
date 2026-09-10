// SPDX-License-Identifier: BSL-1.0
// tests/test_provider_plugin_registry.cpp
//
// §ProviderPlugins Phase 1/2: ProviderRegistry behavior.
//   PR1. The built-in OpenAI-compatible default is always resolvable.
//   PR2. resolve() falls back to the default for unknown names/hosts.
//   PR3. register_plugin() adds a plugin; name lookup is case-insensitive.
//   PR4. Re-registering the same name replaces the previous plugin.
//   PR5. plugin_for_host() uses the plugin's matches() hook.
//   PR6. resolve() prefers explicit name over host matching.
//   PR7. Default plugin hooks shape generic payloads (stream_options
//        only when streaming; MiniMax quirks moved to MiniMaxPlugin).

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

class FakePlugin : public hive::ProviderPlugin {
public:
    std::string name() const override { return "fake-provider"; }
    std::string default_base_url() const override { return "https://fake.example.com"; }
    bool matches(std::string_view host) const override {
        return host.find("fake.example.com") != std::string_view::npos;
    }
};

void test_pr1_default_resolvable() {
    auto def = hive::ProviderRegistry::default_plugin();
    EXPECT(def != nullptr);
    EXPECT(def->name() == "openai-compatible");
    auto by_name = hive::ProviderRegistry::plugin_for_name("OpenAI-Compatible");
    EXPECT(by_name != nullptr);           // case-insensitive
    EXPECT(by_name.get() == def.get());
    std::cout << "[ok] PR1: default plugin resolvable\n";
}

void test_pr2_unknown_falls_back() {
    auto p = hive::ProviderRegistry::resolve("no-such-provider",
                                                 "https://no-such-host.example");
    EXPECT(p != nullptr);
    EXPECT(p->name() == "openai-compatible");
    std::cout << "[ok] PR2: unknown provider falls back to default\n";
}

void test_pr3_register_and_lookup() {
    hive::ProviderRegistry::register_plugin(std::make_shared<FakePlugin>());
    auto p = hive::ProviderRegistry::plugin_for_name("FAKE-Provider");
    EXPECT(p != nullptr);
    EXPECT(p->name() == "fake-provider");
    std::cout << "[ok] PR3: register + case-insensitive lookup\n";
}

void test_pr4_reregister_replaces() {
    class Fake2 : public FakePlugin {};
    auto first = hive::ProviderRegistry::plugin_for_name("fake-provider");
    hive::ProviderRegistry::register_plugin(std::make_shared<Fake2>());
    auto second = hive::ProviderRegistry::plugin_for_name("fake-provider");
    EXPECT(second != nullptr);
    EXPECT(second.get() != first.get());  // replaced, not duplicated
    size_t count = 0;
    for (const auto& p : hive::ProviderRegistry::all()) {
        if (p->name() == "fake-provider") ++count;
    }
    EXPECT(count == 1);
    std::cout << "[ok] PR4: re-register replaces\n";
}

void test_pr5_host_matching() {
    auto p = hive::ProviderRegistry::plugin_for_host("https://fake.example.com/v1");
    EXPECT(p != nullptr);
    EXPECT(p->name() == "fake-provider");
    auto none = hive::ProviderRegistry::plugin_for_host("https://unrelated.example");
    EXPECT(none == nullptr);
    std::cout << "[ok] PR5: host auto-detection via matches()\n";
}

void test_pr6_resolve_prefers_name() {
    // An explicit name wins even when the host would match another plugin.
    auto p = hive::ProviderRegistry::resolve("openai-compatible",
                                                 "https://fake.example.com");
    EXPECT(p != nullptr);
    EXPECT(p->name() == "openai-compatible");
    // Unknown name + matching host -> host match.
    auto q = hive::ProviderRegistry::resolve("whatever", "https://fake.example.com");
    EXPECT(q != nullptr);
    EXPECT(q->name() == "fake-provider");
    std::cout << "[ok] PR6: resolve prefers explicit name\n";
}

void test_pr7_default_payload_shaping() {
    auto def = hive::ProviderRegistry::default_plugin();
    json payload = {{"model", "x"}, {"messages", json::array()}, {"stream", true}};
    hive::ProviderChatContext ctx;
    ctx.model_id = "x";
    ctx.stream = true;
    def->mutate_chat_payload(payload, ctx);
    // §ProviderPlugins Phase 3: the generic default no longer adds the
    // MiniMax quirks (reasoning_split, continuous_usage_stats) — those
    // live in MiniMaxPlugin now. Generic shaping is stream_options only,
    // and only when streaming.
    EXPECT(!payload.contains("reasoning_split"));
    EXPECT(payload.contains("stream_options"));
    EXPECT(payload["stream_options"].value("include_usage", false) == true);
    EXPECT(!payload["stream_options"].contains("continuous_usage_stats"));

    json payload2 = {{"model", "x"}, {"messages", json::array()}, {"stream", false}};
    ctx.stream = false;
    def->mutate_chat_payload(payload2, ctx);
    EXPECT(!payload2.contains("reasoning_split"));
    EXPECT(!payload2.contains("stream_options"));

    // Default clamps/sanitizers are no-ops.
    EXPECT(def->clamp_temperature("x", 0.7) == 0.7);
    json msgs = json::array({{{"role", "system"}, {"content", "a"}}});
    def->sanitize_history(msgs);
    EXPECT(msgs.size() == 1);
    std::cout << "[ok] PR7: default payload shaping preserved\n";
}

} // namespace

int main() {
    test_pr1_default_resolvable();
    test_pr2_unknown_falls_back();
    test_pr3_register_and_lookup();
    test_pr4_reregister_replaces();
    test_pr5_host_matching();
    test_pr6_resolve_prefers_name();
    test_pr7_default_payload_shaping();

    if (failures) {
        std::cerr << "\ntest_provider_plugin_registry: " << failures
                  << " FAILED (of 7 tests)\n";
        return 1;
    }
    std::cout << "\ntest_provider_plugin_registry: 7 tests passed\n";
    return 0;
}
