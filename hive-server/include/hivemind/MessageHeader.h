#pragma once

// =============================================================================
// MessageHeader (2026-10-04): the consistent message header — see Plan.md.
//
// Every message that can wake an agent carries a header as STRUCTURED
// DATA: a `source` (controlled vocabulary) + `created_at` (UTC ISO-8601,
// stamped at message creation). The only formatted-string artifact is
// render_message_header(), used at the agent-prompt edge (and the hub
// journal) — user-facing surfaces never render it; they read the stored
// raw body and, when they want channel styling, the structured fields.
// =============================================================================

#include <string>
#include <string_view>

namespace hivemind {

/// Controlled vocabulary for Delivery::source and the message `source`
/// field. Producers set ONE of these at the message-construction choke
/// point (ChatService::fan_in / the message route); nothing downstream
/// invents new values.
namespace msg_source {
inline constexpr std::string_view kUser        = "user";
inline constexpr std::string_view kVoiceApp    = "voice-app";
inline constexpr std::string_view kPhone       = "phone";
inline constexpr std::string_view kSystem      = "system";
inline constexpr std::string_view kRoutinePrefix = "routine:";  // + label
inline constexpr std::string_view kAgentPrefix   = "agent:";    // + name
/// Rows written before this column existed (migration default).
inline constexpr std::string_view kUnknown     = "unknown";
}  // namespace msg_source

/// True when `source` is a member of the controlled vocabulary:
/// user | voice-app | phone | system | routine:<label> | agent:<name>.
/// The two prefix forms accept any non-empty suffix (the label/name).
inline bool is_valid_message_source(std::string_view source) {
    using namespace msg_source;
    if (source == kUser || source == kVoiceApp || source == kPhone ||
        source == kSystem)
        return true;
    for (const std::string_view prefix : {kRoutinePrefix, kAgentPrefix}) {
        if (source.substr(0, prefix.size()) == prefix &&
            source.size() > prefix.size())
            return true;
    }
    return false;
}

/// Renders the agent-facing display form of a message header.
/// Input fields MUST already be stored data (source, created_at) —
/// this derives text from structured fields; it creates nothing.
///   "[voice-app @ 2026-10-03T22:17:23Z] "
inline std::string render_message_header(std::string_view source,
                                         std::string_view utc_iso) {
    return "[" + std::string(source) + " @ " + std::string(utc_iso) + "] ";
}

/// Channel hint appended to voice-app wakes (Plan.md §3.4): moved
/// server-side from SpeechText.OUTGOING_PREFIX so the directive is
/// attached exactly where the header is rendered — and never leaks into
/// stored bodies or user-facing UIs (it was previously visible in every
/// client as part of the message text).
inline constexpr std::string_view kVoiceChannelHint =
    "(Voice message via the HiveMind Voice app — your reply is read "
    "aloud by text-to-speech, so answer in plain flowing prose: no "
    "markdown, lists, tables, code blocks, links, or emoji; write "
    "numbers, dates and abbreviations the way they should be spoken; "
    "keep it conversational and concise unless I ask for detail. "
    "If you need another agent's input, use message_agent to consult "
    "them, then report the answer back to me via message_user.)\n";

}  // namespace hivemind
