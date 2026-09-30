#pragma once

#include <nlohmann/json.hpp>
#include <string>

namespace mcp {

/// Sanitize an MCP-discovered tool inputSchema for strict provider-side
/// validators (2026-09-30, 4th schema incident: comfy-mcp's
/// set_workflow_slot advertised properties.overrides.items.anyOf whose
/// second option was an internal `$ref: #/$defs/SlotOverride` — Moonshot
/// rejected EVERY request the agent made with "not a valid moonshot
/// flavored json schema: detected infinite recursion without termination
/// condition", silently muting the agent for hours; deliveries recorded
/// done:true with empty replies).
///
/// The provider-side schema only guides the LLM's argument construction —
/// the MCP server validates arguments itself — so rewriting hostile shapes
/// is safe and keeps the tool usable, which is strictly better than
/// poisoning the whole request or dropping the tool outright.
///
/// Phase 1 eliminates "$ref" entirely: internal refs (#/...) are INLINED
/// with cycle detection (verified accepted by the live provider), while
/// unresolvable shapes — external refs, refs with sibling keys, ref
/// cycles, dangling refs — degrade to a permissive {} ("any value").
/// Def tables ($defs / definitions) are stripped once refs are gone.
/// Phase 2 structurally degrades combinator nodes (anyOf/oneOf/allOf)
/// whose serialized form already appears in their own ancestry (a
/// fixpoint that unrolls infinitely) and any nesting deeper than 16.
///
/// Returns true when the schema was modified (caller logs ONE warning
/// per tool at ingestion time).
bool sanitize_tool_schema(nlohmann::json& schema);

} // namespace mcp
