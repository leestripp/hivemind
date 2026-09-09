#pragma once

#include <string>

// Static HTML → plain text for agent consumption (web_fetch).
// Ported from MonkeyByte (monkeybyte/backend/HtmlToText) 2026-09-09.
class HtmlToText {
public:
    // Drops <script>/<style>/<noscript> incl. their content, removes all
    // tags (block-level closers become newlines), decodes common HTML
    // entities, collapses runs of blank lines to one, trims the result.
    static std::string to_text(const std::string& html);
};
