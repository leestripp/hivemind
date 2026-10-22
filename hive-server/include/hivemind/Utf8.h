#pragma once

// =============================================================================
// Utf8 (2026-10-22 bug sweep, B6): byte-level UTF-8 hygiene for every text
// field that enters the hub's messaging stores. See Plan.md and the
// 2026-10-02 poison-wake incident: ONE invalid byte in a stored body
// kills every later nlohmann dump of that body (type_error.316), which
// took down dispatch for an entire agent (head-of-line blocking).
//
// Rule (code-craft-cpp): std::string IS BYTES. Anything from a client,
// a worker frame, or a notification is untrusted text — repair it at
// the store ingress (this header), not at every consumer downstream.
// =============================================================================

#include <cstddef>
#include <string>
#include <string_view>

namespace hivemind {

/// Repairs a UTF-8 byte string by dropping malformed bytes: stray
/// continuation bytes, truncated lead bytes, out-of-range leads
/// (0xC0/0xC1/0xF5-0xFF) and overlong/surrogate encodings. Valid
/// sequences pass through untouched. The result always round-trips
/// through a strict UTF-8 validator, so nlohmann dump can never throw
/// on it. Invalid bytes INSIDE a sequence cause the whole sequence to
/// be re-scanned from the following byte (a dropped lead takes its
/// continuations with it when they are no longer valid on their own).
inline std::string sanitize_utf8(std::string_view in) {
    std::string out;
    out.reserve(in.size());
    std::size_t i = 0;
    const auto n = in.size();
    while (i < n) {
        const auto c = static_cast<unsigned char>(in[i]);
        if (c < 0x80) {                       // ASCII fast path
            out.push_back(static_cast<char>(c));
            ++i;
            continue;
        }
        // Expected total length for the lead byte.
        std::size_t len;
        if (c >= 0xC2 && c <= 0xDF)      len = 2;
        else if (c >= 0xE0 && c <= 0xEF) len = 3;
        else if (c >= 0xF0 && c <= 0xF4) len = 4;
        else { ++i; continue; }               // stray continuation / invalid lead
        if (i + len > n) break;               // truncated tail — drop it
        bool ok = true;
        for (std::size_t k = 1; k < len; ++k) {
            const auto cc = static_cast<unsigned char>(in[i + k]);
            if ((cc & 0xC0) != 0x80) { ok = false; break; }
        }
        // Reject overlong 3-byte (E0 needs A0+) and surrogates
        // (ED needs <A0), and overlong 4-byte / beyond-U+10FFFF
        // (F0 needs 90+, F4 needs <90).
        if (ok && len == 3) {
            const auto c1 = static_cast<unsigned char>(in[i + 1]);
            if ((c == 0xE0 && c1 < 0xA0) || (c == 0xED && c1 >= 0xA0))
                ok = false;
        } else if (ok && len == 4) {
            const auto c1 = static_cast<unsigned char>(in[i + 1]);
            if ((c == 0xF0 && c1 < 0x90) || (c == 0xF4 && c1 >= 0x90))
                ok = false;
        }
        if (ok) {
            out.append(in.substr(i, len));
            i += len;
        } else {
            ++i;                              // drop the bad lead, rescan
        }
    }
    return out;
}

}  // namespace hivemind
