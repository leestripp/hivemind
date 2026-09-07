/**
 * @file hive-brain/AgentState.h
 * @brief High-level state of the Agent. State is data, not formatted strings.
 *
 * Hosts can render this however they want (CLI text, GTK label, web SSE event).
 */
#pragma once

#include <cstdint>
#include <string_view>

namespace hive {

enum class AgentState : std::uint8_t {
    Idle,             ///< Constructed, no work in flight
    Understanding,    ///< ANALYSE phase
    Planning,         ///< PLANNING phase (research + agent's per-project planning doc; library is neutral about the filename — Claude Code uses CLAUDE.md, others use Agent.md, etc.)
    Acting,           ///< ACT phase (tools, edits)
    Verifying,        ///< BUILD_TEST phase
    Documenting,      ///< Documentation / changelog / README updates
    AwaitingApproval, ///< bash approval in flight (sink->request_command_approval blocked)
    Compressing,      ///< Context compression in progress
    Done,             ///< Agent emitted stop() cleanly
    Cancelled,        ///< Cancelled by host (cancel() or stop_token)
    Error,            ///< Unrecoverable error
};

/// @brief Human-readable name (e.g. "Acting"). Never returns nullptr.
const char* to_string(AgentState state) noexcept;

/// @brief Inverse of to_string.  Returns AgentState::Idle (with `ok=false`)
///        when the name is not recognised.  Matching is case-insensitive.
AgentState agent_state_from(std::string_view name, bool* ok = nullptr) noexcept;

}  // namespace hive