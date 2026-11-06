// SpeechText — text hygiene for TTS. 1:1 port of the Android
// SpeechText.kt: punctuation-clause splitting, ellipsis/wrapper/digit
// expansion, code/URL skipping, sentence splitting for the TTS queue.
// Pure functions only — everything here is unit-tested.
#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace speech {

// Spoken form of a matched token (e.g. "…" -> " ... "). Returns "" to
// SKIP the token (drop it from the utterance).
std::string spokenForm(std::string_view tok);

// One speakable clause with a visible end-punctuation hint.
struct Clause { std::string text; bool terminal; };

// Split raw assistant text into clauses. Code fences/URLs/matched
// wrappers are dropped; math/code spans are spelled; bare URLs skipped.
std::vector<Clause> splitClauses(std::string_view raw);

// Join clauses into whole sentences for the TTS queue: a clause ending
// in terminal punctuation (. ! ? … : ;) closes a sentence; a comma
// closes one only when the accumulated text is long enough.
std::vector<std::string> toSentences(const std::vector<Clause>& clauses);

// Convenience: raw text -> sentence queue.
std::vector<std::string> toSpeechSentences(std::string_view raw);


// Convenience: raw text -> one speakable string (markdown stripped,
// fillers dropped). Used for full-note playback.
std::string forSpeech(std::string_view raw);

// Full-message playback: preserve sentence-sized queueing, splitting only
// oversized sentences. Splits prefer whitespace and never cut a UTF-8
// sequence. maxBytes is a byte budget, not a Unicode character count.
std::vector<std::string> toSpeechChunks(std::string_view raw,
                                        std::size_t maxBytes = 2000);

// Length of the common prefix of a and b (used to avoid re-speaking
// already-spoken streamed text).
std::size_t commonPrefixLen(std::string_view a, std::string_view b);

}  // namespace speech
