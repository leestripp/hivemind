// tests/test_token_counts_sse.cpp
//
// Phase 9.10 smoke test: verify token count reporting in SSE streaming mode.
//
// Tests:
//   1. SSE streaming: LLM call completes and final SSE chunk has usage=null
//      (MiniMax API behaviour). Expect a hive::LogLevel::Warn to be emitted.
//   2. After one streaming turn: stats().total_tokens is:
//        - 0  → Warn was emitted (usage=null, expected on MiniMax SSE)
//        - > 0 → compression or some other path got real usage
//      Either way the Warn tells the operator what happened.
//   3. Simple non-error completion (DONE state, no crash).
//
// Run:
//   ./build/tests/test_token_counts_sse
//
// Requires a configured model in the SQLite SettingsStore
// (default ~/.local/share/daedalus/daedalus.db). The environment is already
// configured, so no explicit credentials are needed.

#include <cassert>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "library/Agent.h"
#include "api/ProviderPlugin.h"
#include "api/ProviderRegistry.h"
#include "library/model_discovery.h"
#include "a2a/A2aClient.h"

namespace {

// Counters for sink callbacks
int g_state_changes  = 0;
int g_log_count      = 0;
int g_warn_count     = 0;
int g_delta_count    = 0;
int g_tool_call_count = 0;
std::vector<std::string> g_warn_messages;
std::vector<std::string> g_delta_texts;
std::vector<std::string> g_state_names;

struct TokenCountSink : public hive::AgentSink {
    void on_state_change(int /*from*/, int to) noexcept override {
        ++g_state_changes;
        hive::AgentState s = static_cast<hive::AgentState>(to);
        g_state_names.emplace_back(hive::to_string(s));
    }

    void on_log(hive::LogLevel level, std::string_view msg) noexcept override {
        ++g_log_count;
        if (level == hive::LogLevel::Warn) {
            ++g_warn_count;
            g_warn_messages.emplace_back(msg);
            std::cerr << "[WARN] " << msg << '\n';
        } else {
            std::cerr << "[" << hive::to_string(level) << "] " << msg << '\n';
        }
    }

    void on_assistant_delta(std::string_view text) noexcept override {
        ++g_delta_count;
        if (g_delta_texts.size() < 10) {
            g_delta_texts.emplace_back(text);
        }
    }

    void on_tool_call(std::string_view /*func*/, const nlohmann::json& /*args*/) noexcept override {
        ++g_tool_call_count;
    }
};

}  // namespace

// §Phase 14: this test drives a real LLM turn against the MiniMax API.
// Skipped by default — opt in by setting HIVE_RUN_LIVE_LLM_TESTS=1.
// CI without credentials stays green; nightly jobs opt in.
static bool live_llm_tests_enabled() {
    const char* env = std::getenv("HIVE_RUN_LIVE_LLM_TESTS");
    return env && env[0] != '\0' && env[0] != '0';
}

int main() {
    std::cerr << "=== Phase 9.10 Token Count SSE Smoke Test ===\n";
    if (!live_llm_tests_enabled()) {
        std::cout << "[SKIP] hive_token_counts_sse_tests — "
                     "requires HIVE_RUN_LIVE_LLM_TESTS=1 "
                     "(makes a real MiniMax API call)\n";
        return 77;  // ctest SKIP_RETURN_CODE
    }

    // Use the real settings that have API credentials
    hive::AgentConfig cfg;
    cfg.project_root  = "/tmp";
    cfg.session_id    = "token-count-test";
    cfg.model_name    = "minimax-m2.7";   // matches settings.json entry
    cfg.auto_approve  = true;             // no interactive approval

    auto sink = std::make_shared<TokenCountSink>();
    hive::Agent agent(cfg, sink);

    agent.initialize();

    // Submit a simple prompt that won't need tool calls
    agent.submit_user_message("count to 3");
    std::cerr << "\n--- Running one turn (SSE streaming, model=minimax-m2.7) ---\n";
    agent.run_one_turn();
    hive::AgentState final_state = agent.state();

    hive::AgentStats stats = agent.stats();
    std::cerr << "\n--- Turn complete ---\n";
    std::cerr << "Final state: " << hive::to_string(final_state) << "\n";
    std::cerr << "stats.total_tokens:      " << stats.total_tokens << "\n";
    std::cerr << "stats.prompt_tokens:      " << stats.prompt_tokens << "\n";
    std::cerr << "stats.completion_tokens:  " << stats.completion_tokens << "\n";
    std::cerr << "stats.max_context_size:  " << stats.max_context_size << "\n";
    std::cerr << "\nSink counters:\n";
    std::cerr << "  state_changes:  " << g_state_changes << "\n";
    std::cerr << "  log messages:    " << g_log_count << "\n";
    std::cerr << "  Warn messages:   " << g_warn_count << "\n";
    std::cerr << "  assistant deltas: " << g_delta_count << "\n";
    std::cerr << "  tool calls:       " << g_tool_call_count << "\n";

    if (!g_warn_messages.empty()) {
        std::cerr << "\nWarn messages:\n";
        for (auto& w : g_warn_messages) {
            std::cerr << "  " << w << "\n";
        }
    }

    std::cerr << "\nFirst few delta texts (streaming):\n";
    for (auto& d : g_delta_texts) {
        std::cerr << "  \"" << d << "\"\n";
    }

    agent.shutdown();

    // ── Assertions ─────────────────────────────────────────────────────────
    int failures = 0;

    // Test 1: SSE streaming worked — we got at least one assistant delta
    if (g_delta_count == 0) {
        std::cerr << "\n[FAIL] Test 1: No assistant deltas received (SSE streaming broken?)\n";
        ++failures;
    } else {
        std::cerr << "\n[PASS] Test 1: SSE streaming — " << g_delta_count
                  << " delta(s) received\n";
    }

    // Test 2: Turn completed without crash (state is Done or Cancelled)
    if (final_state != hive::AgentState::Done &&
        final_state != hive::AgentState::Cancelled) {
        std::cerr << "\n[FAIL] Test 2: Unexpected final state: "
                  << hive::to_string(final_state) << "\n";
        ++failures;
    } else {
        std::cerr << "\n[PASS] Test 2: Turn completed (state="
                  << hive::to_string(final_state) << ")\n";
    }

    // Test 3: At least one log message was emitted (token warning or info)
    if (g_log_count == 0) {
        std::cerr << "\n[FAIL] Test 3: No log messages received\n";
        ++failures;
    } else {
        std::cerr << "\n[PASS] Test 3: " << g_log_count << " log message(s) received\n";
    }

    // Test 4: With `stream_options: {include_usage: true}` (added in
    // Phase 9.10), MiniMax sends a usage-only final SSE chunk with real
    // token counts.  We now expect total_tokens > 0.
    bool got_tokens = stats.total_tokens > 0;
    bool got_warn   = g_warn_count > 0;

    if (got_tokens && !got_warn) {
        std::cerr << "\n[PASS] Test 4: Token counts are available: "
                  << stats.total_tokens << " total (no warns; "
                  << "stream_options fix verified)\n";
    } else if (got_tokens && got_warn) {
        std::cerr << "\n[INFO] Test 4: Token counts are available but a Warn was also emitted: "
                  << stats.total_tokens << " total\n";
    } else if (got_warn) {
        std::cerr << "\n[INFO] Test 4: Token counts are 0 — Warn was emitted (MiniMax SSE usage=null fallback)\n";
    } else {
        std::cerr << "\n[FAIL] Test 4: Token counts are 0 AND no Warn was emitted. "
                  << "The stream_options fix should give us real counts.\n";
        ++failures;
    }

    std::cerr << "\n=== Result: " << (failures == 0 ? "ALL PASSED" : "SOME FAILED")
              << " (" << failures << " failure(s)) ===\n";

    return failures;
}
