// tests/test_no_content_duplicate.cpp
//
// Phase 9.11 regression test: verify that the assistant's text content
// is emitted EXACTLY ONCE via `on_assistant_delta` during SSE streaming.
//
// Bug context (see prompt.txt, 2026-07-12):
//   After the Phase 9.10 fix (stream_options: {include_usage: true}),
//   the assistant's response was emitted TWICE in the host sink:
//     1. Once during normal streaming deltas (real-time)
//     2. Once again at line 2029 when the pipeline re-emitted the
//        synthesized message's accumulated content.
//
// Fix:
//   Added `MiniMaxSseParser::had_text_deltas()` and gated the line 2029
//   emit on `!had_text_deltas()`.  So:
//     - SSE streamed text → line 2029 emit SKIPPED (no duplicate)
//     - Non-streamed text → line 2029 emit HAPPENS (existing behaviour)
//
// Test methodology:
//   1. Drive one real LLM turn with SSE streaming enabled
//   2. Capture every on_assistant_delta text into a single concatenated buffer
//   3. Ask the model to output a unique 6-character sentinel "QQ7Z91"
//   4. Strip the `` block (model may quote the sentinel in
//      its reasoning, which is NOT a duplicate from the library)
//   5. Count sentinel occurrences in the stripped text — expect exactly 1
//
// Why strip thinking?
//   MiniMax-M2.7 with thinking enabled will sometimes echo the user's
//   prompt verbatim into its reasoning ("reply with exactly: QQ7Z91")
//   before producing the actual answer.  That isn't a library bug —
//   it's the model itself repeating the string.  Stripping the
//   block isolates the actual answer from the model's reasoning.
//
// Run:
//   ./build/tests/hive_no_content_duplicate_tests

#include <cassert>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <string>

#include "library/Agent.h"
#include "api/ProviderPlugin.h"
#include "api/ProviderRegistry.h"
#include "library/model_discovery.h"
#include "a2a/A2aClient.h"

namespace {

// Concatenated assistant text from every on_assistant_delta callback.
std::string g_concat;
int g_delta_count = 0;

struct DedupSink : public hive::AgentSink {
    void on_assistant_delta(std::string_view text) noexcept override {
        ++g_delta_count;
        g_concat.append(text);
    }
};

// Strip `` blocks from a buffer.  MiniMax-M2.7 streams thinking
// content inside `` tags; we don't want that in our deduplication count.
std::string strip_think_blocks(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    size_t pos = 0;
    while (pos < in.size()) {
        size_t open = in.find("<think>", pos);
        if (open == std::string::npos) {
            out.append(in, pos, std::string::npos);
            break;
        }
        out.append(in, pos, open - pos);
        size_t close = in.find("</think>", open);
        if (close == std::string::npos) {
            // Unclosed think tag — bail out without stripping.
            out.append(in, open, std::string::npos);
            break;
        }
        pos = close + std::string("</think>").size();
    }
    return out;
}

}  // namespace

// §Phase 14: this test drives a real LLM turn against the MiniMax API.
// Skipped by default — opt in by setting HIVE_RUN_LIVE_LLM_TESTS=1.
// CI without credentials stays green; nightly jobs opt in.
static bool live_llm_tests_enabled() {
    const char* env = std::getenv("HIVE_RUN_LIVE_LLM_TESTS");
    return env && env[0] != '\0' && env[0] != '0';
}

int main() {
    std::cerr << "=== Phase 9.11 No-Content-Duplicate SSE Regression Test ===\n";
    if (!live_llm_tests_enabled()) {
        std::cout << "[SKIP] hive_no_content_duplicate_tests — "
                     "requires HIVE_RUN_LIVE_LLM_TESTS=1 "
                     "(makes a real MiniMax API call)\n";
        return 77;  // ctest SKIP_RETURN_CODE
    }

    hive::AgentConfig cfg;
    cfg.project_root  = "/tmp";
    cfg.session_id    = "no-dup-test";
    cfg.model_name    = "minimax-m2.7";
    cfg.auto_approve  = true;

    auto sink = std::make_shared<DedupSink>();
    hive::Agent agent(cfg, sink);

    agent.initialize();

    // Ask for a deterministic response with a sentinel string we can
    // count occurrences of.  "QQ7Z91" is a 6-char alphanumeric that
    // the model is extremely unlikely to produce naturally except when
    // asked for it.
    agent.submit_user_message(
        "Reply with exactly this string and nothing else: QQ7Z91");
    std::cerr << "\n--- Running one turn (SSE streaming, model=minimax-m2.7) ---\n";
    agent.run_one_turn();

    // Strip thinking blocks before counting (model may echo the sentinel
    // inside its reasoning — that's NOT a library duplicate).
    const std::string stripped = strip_think_blocks(g_concat);

    // Count sentinel occurrences in the stripped text.
    const std::string sentinel = "QQ7Z91";
    int count = 0;
    size_t pos = 0;
    while ((pos = stripped.find(sentinel, pos)) != std::string::npos) {
        ++count;
        pos += sentinel.size();
    }

    std::cerr << "\n--- Sentinel detection ---\n";
    std::cerr << "Total deltas received:   " << g_delta_count << "\n";
    std::cerr << "Concatenated length:     " << g_concat.size() << " bytes\n";
    std::cerr << "Stripped length:         " << stripped.size() << " bytes\n";
    std::cerr << "Sentinel occurrences:    " << count << "  (expected: 1)\n";
    std::cerr << "Raw concatenated text:\n----\n" << g_concat << "\n----\n";
    std::cerr << "Stripped text (used for counting):\n----\n" << stripped << "\n----\n";

    int failures = 0;

    if (count == 1) {
        std::cerr << "\n[PASS] Sentinel appeared exactly once in answer — "
                  << "no duplicate emit.\n";
    } else if (count == 0) {
        std::cerr << "\n[FAIL] Sentinel missing — model didn't follow instructions.\n";
        ++failures;
    } else {
        std::cerr << "\n[FAIL] Sentinel appeared " << count << " times in answer — "
                  << "DUPLICATE EMIT REGRESSION.\n";
        ++failures;
    }

    // Sanity check: we got at least one delta (SSE streaming worked).
    if (g_delta_count == 0) {
        std::cerr << "\n[FAIL] No deltas received — SSE streaming broken.\n";
        ++failures;
    } else {
        std::cerr << "\n[PASS] " << g_delta_count
                  << " delta(s) received — SSE streaming works.\n";
    }

    agent.shutdown();

    std::cerr << "\n=== Result: "
              << (failures == 0 ? "ALL PASSED" : "SOME FAILED")
              << " (" << failures << " failure(s)) ===\n";

    return failures;
}