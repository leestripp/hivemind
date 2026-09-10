// SPDX-License-Identifier: BSL-1.0
// tests/test_context_cache.cpp
//
// §ContextCache (2026-07-31): provider prompt-cache support.
//   CC1. extract_cached_tokens: Moonshot top-level shape.
//   CC2. extract_cached_tokens: MiniMax/OpenAI nested shape; missing
//        fields fold to 0.
//   CC3. upsert_context_status_message: appends a transient trailing
//        system message; a second upsert replaces it exactly once and
//        keeps it LAST even after conversation messages were appended
//        behind it.
//   CC4. upsert strips a legacy embedded Context Status block from the
//        system message (pre-§ContextCache sessions persisted one).
//   CC5. The static prefix invariant: after upserts on two consecutive
//        "iterations", every message before the trailing status message
//        is byte-identical (this is what MiniMax / Kimi prefix caches
//        actually match on).
//   CC6. SystemPromptBuilder::compose never emits a Context Status
//        section (static-prompt invariant).

#include <iostream>
#include <string>

#include <nlohmann/json.hpp>

#include "api/ContextStatus.h"
#include "core/SystemPromptBuilder.h"

using json = nlohmann::json;
namespace cs = hive::context_status;

namespace {

int failures = 0;
#define EXPECT(cond) do { \
    if (!(cond)) { \
        std::cerr << "FAIL " << __FILE__ << ":" << __LINE__ \
                  << " EXPECT(" #cond ")\n"; \
        ++failures; \
    } \
} while (0)

json make_history() {
    return json::array({
        {{"role", "system"},    {"content", "# Session Info\n- **User**: tester\n\n# Role\nBe helpful.\n"}},
        {{"role", "user"},      {"content", "hello"}},
        {{"role", "assistant"}, {"content", "hi there"}},
    });
}

void test_cc1_top_level_shape() {
    // Moonshot / Kimi: usage.cached_tokens (platform.kimi.ai/docs/api/chat).
    json usage = {{"prompt_tokens", 1000}, {"completion_tokens", 50},
                  {"total_tokens", 1050}, {"cached_tokens", 800}};
    EXPECT(cs::extract_cached_tokens(usage) == 800);
    std::cout << "[ok] CC1: Moonshot top-level cached_tokens\n";
}

void test_cc2_nested_shape_and_absent() {
    // MiniMax / OpenAI: usage.prompt_tokens_details.cached_tokens.
    json usage = {{"prompt_tokens", 2000}, {"completion_tokens", 60},
                  {"total_tokens", 2060},
                  {"prompt_tokens_details", {{"cached_tokens", 1500}}}};
    EXPECT(cs::extract_cached_tokens(usage) == 1500);

    // No cache fields at all -> 0 (miss / unsupported).
    json plain = {{"prompt_tokens", 10}, {"completion_tokens", 5},
                  {"total_tokens", 15}};
    EXPECT(cs::extract_cached_tokens(plain) == 0);

    // prompt_tokens_details present but not an object -> no crash, 0.
    json weird = {{"prompt_tokens", 10},
                  {"prompt_tokens_details", nullptr}};
    EXPECT(cs::extract_cached_tokens(weird) == 0);

    // Non-object usage -> 0.
    EXPECT(cs::extract_cached_tokens(json(nullptr)) == 0);
    std::cout << "[ok] CC2: nested shape + absent fields\n";
}

void test_cc3_upsert_appends_and_replaces() {
    json messages = make_history();

    cs::upsert_context_status_message(messages, "# Context Status\n| Iterations | 1/100 |\n");
    EXPECT(messages.size() == 4);
    EXPECT(messages.back().value("role", "") == "system");
    EXPECT(messages.back().value("content", "").find(cs::kBeginMarker) == 0);
    EXPECT(messages.back().value("content", "").find("1/100") != std::string::npos);

    // Mid-turn: assistant reply + user message land AFTER the status
    // message in the store.
    messages.push_back({{"role", "assistant"}, {"content", "working..."}});
    messages.push_back({{"role", "user"}, {"content", "continue"}});

    cs::upsert_context_status_message(messages, "# Context Status\n| Iterations | 2/100 |\n");

    // Exactly one status message, and it is last.
    int status_count = 0;
    for (const auto& m : messages) {
        if (m.value("content", "").find(cs::kBeginMarker) != std::string::npos)
            ++status_count;
    }
    EXPECT(status_count == 1);
    EXPECT(messages.back().value("content", "").find("2/100") != std::string::npos);
    // The conversation history survived, in order.
    EXPECT(messages[1].value("content", "") == "hello");
    EXPECT(messages[messages.size() - 2].value("content", "") == "continue");
    std::cout << "[ok] CC3: upsert appends + replaces trailing status message\n";
}

void test_cc4_strips_legacy_embedded_block() {
    // Pre-§ContextCache sessions persisted the stats block inside the
    // system message (H1 form, §B8 sentinel-terminated).
    json messages = json::array({
        {{"role", "system"}, {"content",
            "# Session Info\n- **User**: tester\n\n"
            "# Context Status\n"
            "<!-- This section is auto-updated each iteration -->\n"
            "| Iterations | 3/100 |\n"
            "<!-- /hive-agent-context-status -->\n"
            "# Role\nBe helpful.\n"}},
        {{"role", "user"}, {"content", "hello"}},
    });

    cs::upsert_context_status_message(messages, "# Context Status\n| Iterations | 4/100 |\n");

    const std::string sys = messages[0].value("content", "");
    EXPECT(sys.find("# Context Status") == std::string::npos);
    EXPECT(sys.find(cs::kEndSentinel) == std::string::npos);
    EXPECT(sys.find("# Role") != std::string::npos);
    EXPECT(sys.find("Be helpful.") != std::string::npos);
    // New block exists only as the trailing message.
    EXPECT(messages.back().value("content", "").find(cs::kBeginMarker) == 0);
    std::cout << "[ok] CC4: legacy embedded block stripped from system prompt\n";
}

void test_cc5_static_prefix_invariant() {
    // Two consecutive iterations: everything before the trailing status
    // message must be byte-identical — the property provider prefix
    // caches rely on.
    json a = make_history();
    cs::upsert_context_status_message(a, "# Context Status\n| Iterations | 1/100 |\n");

    json b = a;
    b.push_back({{"role", "assistant"}, {"content", "answer"}});
    cs::upsert_context_status_message(b, "# Context Status\n| Iterations | 2/100 |\n");

    // a without its status message == b without its trailing status
    // message and without the newest assistant message.
    json a_prefix = a;
    a_prefix.erase(a_prefix.size() - 1);
    json b_prefix = b;
    b_prefix.erase(b_prefix.size() - 1);  // status
    b_prefix.erase(b_prefix.size() - 1);  // newest assistant message
    EXPECT(a_prefix.dump() == b_prefix.dump());
    std::cout << "[ok] CC5: static prefix invariant across iterations\n";
}

void test_cc6_compose_has_no_stats_section() {
    SystemPromptOptions options;
    options.system_prompt = "Host text.";
    std::string prompt = SystemPromptBuilder::compose(options);
    EXPECT(prompt.find("# Context Status") == std::string::npos);
    EXPECT(prompt.find("Token Usage") == std::string::npos);
    std::cout << "[ok] CC6: composed prompt is stats-free (byte-static)\n";
}

} // namespace

int main() {
    test_cc1_top_level_shape();
    test_cc2_nested_shape_and_absent();
    test_cc3_upsert_appends_and_replaces();
    test_cc4_strips_legacy_embedded_block();
    test_cc5_static_prefix_invariant();
    test_cc6_compose_has_no_stats_section();
    if (failures > 0) {
        std::cerr << failures << " failure(s)\n";
        return 1;
    }
    std::cout << "all context cache tests passed\n";
    return 0;
}
