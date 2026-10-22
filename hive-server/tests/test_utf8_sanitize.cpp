// tests/test_utf8_sanitize.cpp — UTF-8 repair at the messaging ingress
// (2026-10-22 bug sweep, B6): hivemind::sanitize_utf8 must strip invalid
// bytes (2026-10-02 poison-wake incident — one invalid byte in a stored
// body kills every later nlohmann dump with type_error.316).
#include "hivemind/Utf8.h"

#include <cassert>
#include <iostream>
#include <string>

int main() {
    using hivemind::sanitize_utf8;
    using S = std::string;

    // ASCII passes through unchanged.
    assert(sanitize_utf8("hello world") == "hello world");
    assert(sanitize_utf8("").empty());

    // Valid multi-byte sequences (2/3/4-byte) survive intact.
    const S u8 = "\xC3\xA9\xE2\x82\xAC\xF0\x9F\x98\x80";  // é € 😀
    assert(sanitize_utf8(u8) == u8);

    // Lone continuation bytes are dropped.
    assert(sanitize_utf8(S{"a\x80"}) == "a");
    assert(sanitize_utf8(S{"\xBF\xBF"}) == "");

    // A truncated lead byte at the end of the string is dropped.
    assert(sanitize_utf8(S{"abc\xE2"}) == "abc");
    assert(sanitize_utf8(S{"x\xF0\x9F"}) == "x");

    // The exact poison shape from the 2026-10-02 incident: a zero-width
    // block truncated mid-sequence (…\xE2 at the tail).
    const S poison = S{"news \xE2\x80\x8B\xE2\x80\x8Btail\xE2"};
    assert(sanitize_utf8(poison) == S{"news \xE2\x80\x8B\xE2\x80\x8Btail"});

    // An invalid lead byte (0xFF) is dropped along with the continuation
    // byte that followed it.
    assert(sanitize_utf8(S{"a\xFF\x80zb"}) == "azb");

    // A valid sequence right after a dropped bad byte is preserved.
    assert(sanitize_utf8(S{"\xFF\xC3\xA9"}) == S{"\xC3\xA9"});

    std::cout << "test_utf8_sanitize: ALL PASS\n";
    return 0;
}
