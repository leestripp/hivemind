#pragma once

// text_utils — pure text/markup helpers factored out of HiveWindow.cpp so
// they can be unit-tested without a GTK main loop (Phase: client tests).
// Only glibmm (Markup::escape_text) and nlohmann_json are required.

#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace hive::text {

bool starts_with(const std::string& s, const std::string& p);

// Remove "[tool: …]" call lines and "→ {…}" result lines from a hive-brain
// turn transcript; collapse the blank runs they leave behind.
std::string strip_tool_transcript(const std::string& body);

// Split text on ``` fences into prose / code segments.
struct Seg { bool code; std::string text; };
std::vector<Seg> split_segments(const std::string& text);

// Minimal inline markdown → Pango markup: **bold**, *italic*, `code`.
std::string inline_markup(const std::string& line);

// Line-level markdown → Pango markup: #/##/### headers, "- " bullets,
// plus inline_markup per line.
std::string prose_to_markup(const std::string& text);

// "2026-08-23T06:09:43.6Z" → "06:09 UTC" (same UTC day) or
// "Aug 22, 22:14 UTC". All timestamps are UTC by design — including the
// "today" comparison (gmtime, not localtime).
std::string format_msg_time(const std::string& iso);

// Stable per-agent palette index (0..5) — the name label color.
int palette_index(const std::string& name);

// Agent display name from a roster array; falls back to the raw id.
std::string agent_name_of(const nlohmann::json& agents, const std::string& id);

// RFC 3986 percent-encoder for query values.
std::string url_escape(const std::string& s);

} // namespace hive::text
