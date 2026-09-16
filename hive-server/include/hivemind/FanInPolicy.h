#pragma once

// FanInPolicy (Phase 65c/65h): PURE functions deciding whether an agent
// reply should spawn new turns for thread peers. Kept free of ChatService
// state so the heuristics are unit-testable in isolation.
//
// Background (2026-09-16 storm): complete_turn fanned EVERY non-empty
// reply to ALL thread peers. In >2-agent threads the branching factor is
// N-1 — one message became 3+9+27 tasks across the generation cap and
// four agents acknowledging each other ran 42 messages/hour. 2-agent
// threads have factor 1 and self-damp, so they keep broadcast behaviour.

#include <string>
#include <utility>
#include <vector>

namespace hivemind {

/// 65c — true when a reply is pure acknowledgment/status narration that
/// should POST to the thread (visibility) but must NOT fan in to peers.
/// Conservative: >=2 markers always qualify; exactly 1 marker only when
/// the body is short. When in doubt returns false (fan in as before).
bool fanin_is_ack_only(const std::string& body);

/// 65h — parse explicit addressing in a reply: @Name or [to:Name]
/// (case-insensitive, word-bounded). `id_names` = (agent_id, agent_name)
/// pairs of candidate targets (thread participants minus the speaker).
/// Returns matching agent ids in candidate order, deduped.
/// On any doubt returns an empty vector — a missed handoff is
/// recoverable (the target still sees the thread); an accidental
/// broadcast is the storm.
std::vector<std::string> fanin_mentioned_ids(
    const std::string& body,
    const std::vector<std::pair<std::string, std::string>>& id_names);

} // namespace hivemind
