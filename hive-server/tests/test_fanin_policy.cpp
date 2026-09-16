// tests/test_fanin_policy.cpp — Phase 65c/65h pure-function tests for
// the fan-in heuristics (ack suppression + mention parsing).
#include "hivemind/FanInPolicy.h"

#include <cassert>
#include <iostream>

using hivemind::fanin_is_ack_only;
using hivemind::fanin_mentioned_ids;

int main() {
    // ── ack-only detection ─────────────────────────────────────────────
    // Real storm messages (2026-09-16):
    assert(fanin_is_ack_only(
        "Buzz's stop-acknowledgment is addressed to the team — no response "
        "required from me, and any reply would be exactly the turn-generation "
        "Lee banned. Full silence. State is banked and self-running."));
    assert(fanin_is_ack_only(
        "Context heartbeat at 28.5%, no message. Lee's stop order stands: "
        "full silence, no actions. State banked, routines armed."));
    assert(fanin_is_ack_only(
        "Another stale post-compression echo — session fully closed, machine "
        "self-running, nothing requested. State matches my records exactly."));
    assert(fanin_is_ack_only("Thanks, Lee! 🐝 Stopped. Going silent."));
    assert(fanin_is_ack_only("Acknowledged — closing quietly."));

    // Substantive replies must NOT be suppressed, even with one marker:
    std::string long_real = "Acknowledged — and here is the full analysis "
        "you asked for. ";
    long_real += std::string(900, 'x');   // long + single marker → fans
    assert(!fanin_is_ack_only(long_real));
    assert(!fanin_is_ack_only(
        "Here are the three gap-register updates, merged and verified on "
        "disk: register v2 now carries Buzz's ICP section..."));
    assert(!fanin_is_ack_only(""));

    // ── mention parsing ────────────────────────────────────────────────
    const std::vector<std::pair<std::string, std::string>> roster = {
        {"id-buzz",  "Buzz"},
        {"id-pa",    "PA"},
        {"id-honey", "Honey"},
    };

    // @Name form
    auto ids = fanin_mentioned_ids("@Buzz can you verify the price sheet?",
                                   roster);
    assert(ids.size() == 1 && ids[0] == "id-buzz");

    // [to:Name] form, case-insensitive, multiple targets
    ids = fanin_mentioned_ids("[to:pa, Honey] drafts are queued.", roster);
    assert(ids.size() == 2);

    // No addressing → NOBODY (the storm-safe default)
    ids = fanin_mentioned_ids("Team update: register v2 is merged.", roster);
    assert(ids.empty());

    // Bare name in prose is NOT addressing
    ids = fanin_mentioned_ids("Buzz's follow-up is the reconciliation.",
                              roster);
    assert(ids.empty());

    // Word boundaries: @Buzzword must not match Buzz
    ids = fanin_mentioned_ids("@Buzzword is not a roster agent.", roster);
    assert(ids.empty());

    // Unknown mention → nobody
    ids = fanin_mentioned_ids("@Zed hello", roster);
    assert(ids.empty());

    std::cout << "test_fanin_policy: all assertions passed\n";
    return 0;
}
