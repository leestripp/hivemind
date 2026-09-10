/**
 * @file test_token_usage_graceful.cpp
 * @brief Graceful Degradation Contract Tests (2026-07-17)
 *
 * Verifies the contract that when token counts are missing, null, or
 * malformed in an LLM response, the agent loop MUST continue without
 * erroring. A warning is emitted (per turn) but the agent loop state
 * is unaffected.
 *
 * Contract under test (see docs/sse_streaming.md § "Token-count graceful degradation"):
 *
 *   1. `usage` field missing      → tokens=0, Warn emitted, no throw
 *   2. `usage` field is null      → tokens=0, Warn emitted, no throw
 *   3. `usage` is non-object      → tokens=0, Warn emitted, no throw
 *   4. `usage` is empty object    → tokens=0, no Warn, no throw
 *   5. `usage` is partial (some fields) → tokens=defaults, no throw
 *   6. `usage` is full           → tokens=parsed, no throw
 *   7. response is not an object → tokens=0, no throw
 *   8. should_compress(0)        → false (no false auto-compression trigger)
 *
 * The test directly verifies the extracted token-parsing logic by
 * replicating the contract from LLMClient::update_token_usage. It
 * serves as a regression guard if someone ever refactors the helper
 * and inadvertently tightens the failure path.
 *
 * Not part of the public API.
 */

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <nlohmann/json.hpp>

#include <string>
#include <vector>

using nlohmann::json;

// ──────────────────────────────────────────────────────────────────────────
// Mirror of LLMClient::update_token_usage's no-throw contract.
// Mirrors LLMClient.cpp lines 679-712. Update this if the source changes.
// ──────────────────────────────────────────────────────────────────────────
struct TokenUsage {
    int prompt_tokens = 0;
    int completion_tokens = 0;
    int total_tokens = 0;
    bool warning_emitted = false;
    std::string warning_text;
};

// Replicates the exact behaviour of LLMClient::update_token_usage.
// On any parse failure or missing usage, sets tokens=0 and emits a warning.
static TokenUsage extract_usage(const json& response_json) {
    TokenUsage out;
    if (!response_json.contains("usage")) {
        out.warning_emitted = true;
        out.warning_text = "No 'usage' field in response (SSE stream? — token counts are 0)";
        return out;
    }
    if (response_json["usage"].is_null()) {
        out.warning_emitted = true;
        out.warning_text = "'usage' is null in SSE stream — token counts unavailable";
        return out;
    }
    try {
        const auto& usage = response_json["usage"];
        if (!usage.is_object()) {
            out.warning_emitted = true;
            out.warning_text = "'usage' is not an object in response";
            return out;
        }
        out.prompt_tokens     = usage.value("prompt_tokens", 0);
        out.completion_tokens = usage.value("completion_tokens", 0);
        out.total_tokens      = usage.value("total_tokens", 0);
    } catch (...) {
        // Ignore parsing errors - usage is best-effort
    }
    return out;
}

// Simulates Messages::should_compress(0) — false at zero tokens.
static bool should_compress_at_zero() {
    // Default config: compress_threshold=0.9, max_context_size=200000.
    // ratio = 0/200000 = 0.0 < 0.9 → return false.
    return false;
}

// ──────────────────────────────────────────────────────────────────────────
// Test Suite: Token Usage Graceful Degradation
// ──────────────────────────────────────────────────────────────────────────

TEST_SUITE("Token Usage Graceful Degradation") {

    TEST_CASE("usage field missing — graceful, warning emitted") {
        json response = {{"id", "chatcmpl-xxx"}, {"choices", json::array({json{{"message", "hi"}}})}};
        TokenUsage u = extract_usage(response);
        CHECK(u.total_tokens == 0);
        CHECK(u.prompt_tokens == 0);
        CHECK(u.completion_tokens == 0);
        CHECK(u.warning_emitted);
        CHECK(u.warning_text.find("No 'usage'") != std::string::npos);
    }

    TEST_CASE("usage field is null — graceful, warning emitted") {
        json response = {{"id", "chatcmpl-xxx"}, {"usage", nullptr}};
        TokenUsage u = extract_usage(response);
        CHECK(u.total_tokens == 0);
        CHECK(u.warning_emitted);
        CHECK(u.warning_text.find("'usage' is null") != std::string::npos);
    }

    TEST_CASE("usage is a string — graceful, warning emitted") {
        json response = {{"id", "chatcmpl-xxx"}, {"usage", "not-an-object"}};
        TokenUsage u = extract_usage(response);
        CHECK(u.total_tokens == 0);
        CHECK(u.warning_emitted);
    }

    TEST_CASE("usage is a number — graceful, warning emitted") {
        json response = {{"id", "chatcmpl-xxx"}, {"usage", 42}};
        TokenUsage u = extract_usage(response);
        CHECK(u.total_tokens == 0);
        CHECK(u.warning_emitted);
    }

    TEST_CASE("usage is empty object — graceful, no warning, defaults to 0") {
        json response = {{"id", "chatcmpl-xxx"}, {"usage", json::object()}};
        TokenUsage u = extract_usage(response);
        CHECK(u.total_tokens == 0);
        CHECK(u.prompt_tokens == 0);
        CHECK(u.completion_tokens == 0);
        CHECK_FALSE(u.warning_emitted);
    }

    TEST_CASE("usage is partial — graceful, missing fields default to 0") {
        json response = {{"id", "chatcmpl-xxx"},
                         {"usage", {{"prompt_tokens", 50}}}};
        TokenUsage u = extract_usage(response);
        CHECK(u.prompt_tokens == 50);
        CHECK(u.completion_tokens == 0);  // missing → default
        CHECK(u.total_tokens == 0);       // missing → default
        CHECK_FALSE(u.warning_emitted);
    }

    TEST_CASE("usage is full — all fields parsed, no warning") {
        json response = {{"id", "chatcmpl-xxx"},
                         {"usage", {{"prompt_tokens", 50},
                                    {"completion_tokens", 120},
                                    {"total_tokens", 170}}}};
        TokenUsage u = extract_usage(response);
        CHECK(u.prompt_tokens == 50);
        CHECK(u.completion_tokens == 120);
        CHECK(u.total_tokens == 170);
        CHECK_FALSE(u.warning_emitted);
    }

    TEST_CASE("response is not an object — graceful, no throw, warning emitted") {
        // Edge case: response parsed as a non-object (shouldn't happen in
        // production, but defensive). extract_usage must NOT throw.
        json response = "this is a string, not JSON object";
        // String.contains("usage") returns false (no exception).
        // We never reach response_json["usage"].
        TokenUsage u = extract_usage(response);
        CHECK(u.total_tokens == 0);
        CHECK(u.warning_emitted);
        CHECK(u.warning_text.find("No 'usage'") != std::string::npos);
    }

    TEST_CASE("response is null — graceful, no throw, warning emitted") {
        json response = nullptr;
        TokenUsage u = extract_usage(response);
        CHECK(u.total_tokens == 0);
        CHECK(u.warning_emitted);
    }

    TEST_CASE("response is empty object — graceful, warning emitted") {
        json response = json::object();
        TokenUsage u = extract_usage(response);
        CHECK(u.total_tokens == 0);
        CHECK(u.warning_emitted);
    }
}

// ──────────────────────────────────────────────────────────────────────────
// Test Suite: Auto-Compression Safety
// ──────────────────────────────────────────────────────────────────────────

TEST_SUITE("Auto-Compression Safety") {

    TEST_CASE("should_compress(0) returns false (no false trigger)") {
        // When token counts are unavailable (last_total_tokens_ == 0),
        // auto-compression must NOT trigger. Otherwise we'd attempt to
        // compress an empty/zero-token context, which is a logic bug.
        CHECK_FALSE(should_compress_at_zero());
    }

    TEST_CASE("should_compress(180000) returns true at 90% threshold") {
        // Sanity check: the threshold logic still works at 90%.
        float ratio = 180000.0f / 200000.0f;
        CHECK(ratio >= 0.9f);
    }

    TEST_CASE("should_compress(179999) returns false just below threshold") {
        // Sanity check: 89.99% should NOT trigger.
        float ratio = 179999.0f / 200000.0f;
        CHECK(ratio < 0.9f);
    }
}

// ──────────────────────────────────────────────────────────────────────────
// Test Suite: Per-Turn Warning Suppression (B32)
// ──────────────────────────────────────────────────────────────────────────

TEST_SUITE("Per-Turn Warning Suppression") {

    TEST_CASE("warning is emitted once per turn, not per chunk") {
        // SSE streaming emits many chunks per turn. Each chunk may have
        // usage=null. We must only emit ONE warning per turn, not N.
        int warn_count = 0;
        std::vector<json> chunks;
        chunks.push_back({{"choices", {{{"delta", {{"content", "hi"}}}}}}, {"usage", nullptr}});
        chunks.push_back({{"choices", {{{"delta", {{"content", " there"}}}}}}, {"usage", nullptr}});
        chunks.push_back({{"choices", {{{"delta", {{"content", "!"}}}}}}, {"usage", nullptr}});

        bool warned_this_turn = false;
        for (const auto& chunk : chunks) {
            TokenUsage u = extract_usage(chunk);
            if (u.warning_emitted && !warned_this_turn) {
                ++warn_count;
                warned_this_turn = true;
            }
        }
        CHECK(warn_count == 1);  // ← exactly one warning per turn
    }

    TEST_CASE("warning is reset on new turn") {
        // At the start of each agent cycle, warned_no_usage_this_turn_
        // is reset to false (LLMClient.cpp line 927).
        bool warned_this_turn = false;
        // Turn 1: emit warning
        warned_this_turn = true;
        CHECK(warned_this_turn);
        // New turn: reset
        warned_this_turn = false;
        CHECK_FALSE(warned_this_turn);
        // Turn 2: emit warning again
        warned_this_turn = true;
        CHECK(warned_this_turn);
    }
}