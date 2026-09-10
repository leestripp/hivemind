/**
 * @file test_utf8.cpp
 * @brief Unit tests for UTF-8 validation and manipulation utilities
 * 
 * Uses CMake/CTest for test discovery - no external dependencies.
 */

#include <iostream>
#include <string>
#include <cstring>
#include <cstdlib>
#include "core/Utf8Utils.h"

// ============================================================================
// Test Registration
// ============================================================================
#define TEST(name) int test_##name()

// Helper macros for assertions
#define CHECK_EQ(a, b) do { \
    if ((a) != (b)) { \
        std::cerr << "FAIL: " << #a << " == " << #b << " (" << (a) << " vs " << (b) << ")" << std::endl; \
        return 1; \
    } \
} while(0)

#define CHECK_NE(a, b) do { \
    if ((a) == (b)) { \
        std::cerr << "FAIL: " << #a << " != " << #b << " (both " << (a) << ")" << std::endl; \
        return 1; \
    } \
} while(0)

#define CHECK(cond) do { \
    if (!(cond)) { \
        std::cerr << "FAIL: " << #cond << std::endl; \
        return 1; \
    } \
} while(0)

#define CHECK_FALSE(cond) do { \
    if (cond) { \
        std::cerr << "FAIL: " << #cond << " (should be false)" << std::endl; \
        return 1; \
    } \
} while(0)

#define CHECK_GE(a, b) do { \
    if ((a) < (b)) { \
        std::cerr << "FAIL: " << #a << " >= " << #b << " (" << (a) << " < " << (b) << ")" << std::endl; \
        return 1; \
    } \
} while(0)

#define CHECK_GT(a, b) do { \
    if ((a) <= (b)) { \
        std::cerr << "FAIL: " << #a << " > " << #b << " (" << (a) << " <= " << (b) << ")" << std::endl; \
        return 1; \
    } \
} while(0)

#define CHECK_LT(a, b) do { \
    if ((a) >= (b)) { \
        std::cerr << "FAIL: " << #a << " < " << #b << " (" << (a) << " >= " << (b) << ")" << std::endl; \
        return 1; \
    } \
} while(0)

// ============================================================================
// Test Strings (duplicated from source for standalone testing)
// ============================================================================


// ============================================================================
// UTF-8 Validation Tests
// ============================================================================

TEST(utf8_valid_ascii) {
    CHECK(is_valid_utf8(utf8_test_strings::empty));
    CHECK(is_valid_utf8(utf8_test_strings::ascii_hello));
    CHECK(is_valid_utf8(utf8_test_strings::ascii_special));
    CHECK(is_ascii_only(utf8_test_strings::ascii_hello));
    CHECK(is_ascii_only(utf8_test_strings::ascii_special));
    return 0;
}

TEST(utf8_valid_multi_byte) {
    CHECK(is_valid_utf8(utf8_test_strings::chinese));
    CHECK(is_valid_utf8(utf8_test_strings::japanese));
    CHECK(is_valid_utf8(utf8_test_strings::korean));
    CHECK(is_valid_utf8(utf8_test_strings::emoji));
    CHECK(is_valid_utf8(utf8_test_strings::mixed_ascii_utf8));
    CHECK(is_valid_utf8(utf8_test_strings::french));
    CHECK(is_valid_utf8(utf8_test_strings::german));
    CHECK(is_valid_utf8(utf8_test_strings::spanish));
    CHECK(is_valid_utf8(utf8_test_strings::russian));
    CHECK(is_valid_utf8(utf8_test_strings::arabic));
    CHECK(is_valid_utf8(utf8_test_strings::hebrew));
    CHECK(is_valid_utf8(utf8_test_strings::four_byte_chars));
    CHECK(is_valid_utf8(utf8_test_strings::long_string));
    
    CHECK_FALSE(is_ascii_only(utf8_test_strings::chinese));
    CHECK_FALSE(is_ascii_only(utf8_test_strings::emoji));
    CHECK_FALSE(is_ascii_only(utf8_test_strings::russian));
    return 0;
}

TEST(utf8_invalid_sequences) {
    // Isolated continuation bytes
    std::string isolated_cont;
    isolated_cont += static_cast<char>(0x80);
    CHECK_FALSE(is_valid_utf8(isolated_cont));
    
    // Incomplete 2-byte sequence
    std::string incomplete_2byte = "AB";
    incomplete_2byte += static_cast<char>(0xC0);
    CHECK_FALSE(is_valid_utf8(incomplete_2byte));
    
    // Incomplete 3-byte sequence
    std::string incomplete_3byte = "AB";
    incomplete_3byte += static_cast<char>(0xE0);
    incomplete_3byte += static_cast<char>(0x80);
    CHECK_FALSE(is_valid_utf8(incomplete_3byte));
    
    // Overlong 2-byte
    std::string overlong_2byte;
    overlong_2byte += static_cast<char>(0xC0);
    overlong_2byte += static_cast<char>(0x80);
    CHECK_FALSE(is_valid_utf8(overlong_2byte));
    
    // Overlong 3-byte
    std::string overlong_3byte;
    overlong_3byte += static_cast<char>(0xE0);
    overlong_3byte += static_cast<char>(0x80);
    overlong_3byte += static_cast<char>(0x80);
    CHECK_FALSE(is_valid_utf8(overlong_3byte));
    
    return 0;
}

TEST(utf8_edge_cases) {
    // BOM
    std::string bom;
    bom += static_cast<char>(0xEF);
    bom += static_cast<char>(0xBB);
    bom += static_cast<char>(0xBF);
    bom += 'H';
    bom += 'i';
    CHECK(is_valid_utf8(bom));
    
    // Null bytes
    std::string with_null = std::string("Hi\0Bye", 5);
    CHECK(is_valid_utf8(with_null));
    
    // Mixed high bytes
    std::string mixed = "Hello";
    mixed += static_cast<char>(0xC3);
    mixed += static_cast<char>(0xB1);
    mixed += "World";
    CHECK(is_valid_utf8(mixed));
    
    return 0;
}

// ============================================================================
// UTF-8 Character Length Tests
// ============================================================================

TEST(utf8_char_length_ascii) {
    CHECK_EQ(utf8_char_length(utf8_test_strings::ascii_hello, 0), 1);
    CHECK_EQ(utf8_char_length(utf8_test_strings::ascii_hello, 1), 1);
    CHECK_EQ(utf8_char_length(utf8_test_strings::ascii_hello, 5), 1);
    return 0;
}

TEST(utf8_char_length_extended) {
    std::string latin_ext = "ñ";
    CHECK_EQ(utf8_char_length(latin_ext, 0), 2);
    CHECK_EQ(utf8_char_length(latin_ext, 1), 0);
    
    std::string euro = "€";
    CHECK_EQ(utf8_char_length(euro, 0), 3);
    return 0;
}

TEST(utf8_char_length_cjk) {
    CHECK_EQ(utf8_char_length(utf8_test_strings::chinese, 0), 3);
    CHECK_EQ(utf8_char_length(utf8_test_strings::chinese, 3), 3);
    CHECK_EQ(utf8_char_length(utf8_test_strings::chinese, 6), 3);
    CHECK_EQ(utf8_char_length(utf8_test_strings::chinese, 9), 3);
    return 0;
}

TEST(utf8_char_length_emoji) {
    CHECK_EQ(utf8_char_length(utf8_test_strings::emoji, 0), 4);
    CHECK_EQ(utf8_char_length(utf8_test_strings::emoji, 4), 4);
    return 0;
}

TEST(utf8_char_length_helpers) {
    CHECK_EQ(utf8_char_length_from_first_byte(0x00), 1);
    CHECK_EQ(utf8_char_length_from_first_byte(0x7F), 1);
    CHECK_EQ(utf8_char_length_from_first_byte(0xC0), 2);
    CHECK_EQ(utf8_char_length_from_first_byte(0xDF), 2);
    CHECK_EQ(utf8_char_length_from_first_byte(0xE0), 3);
    CHECK_EQ(utf8_char_length_from_first_byte(0xEF), 3);
    CHECK_EQ(utf8_char_length_from_first_byte(0xF0), 4);
    CHECK_EQ(utf8_char_length_from_first_byte(0xF7), 4);
    CHECK_EQ(utf8_char_length_from_first_byte(0x80), 0);
    CHECK_EQ(utf8_char_length_from_first_byte(0xFF), 0);
    return 0;
}

// ============================================================================
// UTF-8 Character Count Tests
// ============================================================================

TEST(utf8_char_count_ascii) {
    CHECK_EQ(utf8_char_count(utf8_test_strings::empty), 0);
    CHECK_EQ(utf8_char_count(utf8_test_strings::ascii_hello), 13);
    CHECK_GE(utf8_char_count(utf8_test_strings::ascii_special), 20);
    return 0;
}

TEST(utf8_char_count_multi_byte) {
    CHECK_EQ(utf8_char_count(utf8_test_strings::chinese), 4);
    CHECK_GE(utf8_char_count(utf8_test_strings::japanese), 5);
    CHECK_EQ(utf8_char_count(utf8_test_strings::korean), 5);
    CHECK_EQ(utf8_char_count(utf8_test_strings::emoji), 4);
    CHECK_EQ(utf8_char_count(utf8_test_strings::four_byte_chars), 4);
    return 0;
}

TEST(utf8_char_count_mixed) {
    size_t count = utf8_char_count(utf8_test_strings::mixed_ascii_utf8);
    CHECK_GT(count, 0);
    CHECK_LT(count, utf8_test_strings::mixed_ascii_utf8.size());
    return 0;
}

// ============================================================================
// UTF-8 Truncation Tests
// ============================================================================

TEST(utf8_truncate_smaller) {
    std::string result = utf8_truncate(utf8_test_strings::ascii_hello, 5);
    CHECK_EQ(result, "Hello");
    CHECK_EQ(result.size(), 5);
    return 0;
}

TEST(utf8_truncate_ascii_boundary) {
    std::string result = utf8_truncate(utf8_test_strings::ascii_hello, 5);
    CHECK(is_valid_utf8(result));
    CHECK_EQ(utf8_char_count(result), 5);
    return 0;
}

TEST(utf8_truncate_chinese_boundary) {
    std::string result = utf8_truncate(utf8_test_strings::chinese, 6);
    CHECK(is_valid_utf8(result));
    CHECK_EQ(result.size(), 6);
    CHECK_EQ(utf8_char_count(result), 2);
    return 0;
}

TEST(utf8_truncate_emoji_boundary) {
    std::string result = utf8_truncate(utf8_test_strings::emoji, 4);
    CHECK(is_valid_utf8(result));
    CHECK_EQ(result.size(), 4);
    CHECK_EQ(utf8_char_count(result), 1);
    
    result = utf8_truncate(utf8_test_strings::emoji, 8);
    CHECK(is_valid_utf8(result));
    CHECK_EQ(result.size(), 8);
    CHECK_EQ(utf8_char_count(result), 2);
    return 0;
}

TEST(utf8_truncate_zero) {
    std::string result = utf8_truncate(utf8_test_strings::ascii_hello, 0);
    CHECK_EQ(result, "");
    CHECK(is_valid_utf8(result));
    return 0;
}

TEST(utf8_truncate_original_size) {
    std::string result = utf8_truncate(utf8_test_strings::ascii_hello, utf8_test_strings::ascii_hello.size());
    CHECK_EQ(result, utf8_test_strings::ascii_hello);
    return 0;
}

TEST(utf8_truncate_larger_size) {
    std::string result = utf8_truncate(utf8_test_strings::ascii_hello, 100);
    CHECK_EQ(result, utf8_test_strings::ascii_hello);
    return 0;
}

TEST(utf8_truncate_chars_chinese) {
    std::string result = utf8_truncate_chars(utf8_test_strings::chinese, 2);
    CHECK(is_valid_utf8(result));
    CHECK_EQ(utf8_char_count(result), 2);
    CHECK_EQ(result.size(), 6);
    return 0;
}

TEST(utf8_truncate_chars_ascii) {
    std::string result = utf8_truncate_chars(utf8_test_strings::ascii_hello, 5);
    CHECK_EQ(result, "Hello");
    CHECK_EQ(utf8_char_count(result), 5);
    return 0;
}

TEST(utf8_truncate_chars_emoji) {
    std::string result = utf8_truncate_chars(utf8_test_strings::emoji, 2);
    CHECK(is_valid_utf8(result));
    CHECK_EQ(utf8_char_count(result), 2);
    CHECK_EQ(result.size(), 8);
    return 0;
}

// ============================================================================
// UTF-8 Index Conversion Tests
// ============================================================================

TEST(utf8_byte_to_char_ascii) {
    CHECK_EQ(utf8_byte_to_char_index(utf8_test_strings::ascii_hello, 0), 0);
    CHECK_EQ(utf8_byte_to_char_index(utf8_test_strings::ascii_hello, 5), 5);
    CHECK_EQ(utf8_byte_to_char_index(utf8_test_strings::ascii_hello, 12), 12);
    return 0;
}

TEST(utf8_byte_to_char_multi_byte) {
    CHECK_EQ(utf8_byte_to_char_index(utf8_test_strings::chinese, 0), 0);
    CHECK_EQ(utf8_byte_to_char_index(utf8_test_strings::chinese, 3), 1);
    CHECK_EQ(utf8_byte_to_char_index(utf8_test_strings::chinese, 6), 2);
    return 0;
}

TEST(utf8_char_to_byte_ascii) {
    CHECK_EQ(utf8_char_to_byte_index(utf8_test_strings::ascii_hello, 0), 0);
    CHECK_EQ(utf8_char_to_byte_index(utf8_test_strings::ascii_hello, 5), 5);
    return 0;
}

TEST(utf8_char_to_byte_multi_byte) {
    CHECK_EQ(utf8_char_to_byte_index(utf8_test_strings::chinese, 0), 0);
    CHECK_EQ(utf8_char_to_byte_index(utf8_test_strings::chinese, 1), 3);
    CHECK_EQ(utf8_char_to_byte_index(utf8_test_strings::chinese, 2), 6);
    CHECK_EQ(utf8_char_to_byte_index(utf8_test_strings::chinese, 3), 9);
    return 0;
}

// ============================================================================
// UTF-8 Substring Tests
// ============================================================================

TEST(utf8_substr_basic) {
    std::string result = utf8_substr(utf8_test_strings::chinese, 0, 2);
    CHECK(is_valid_utf8(result));
    CHECK_EQ(utf8_char_count(result), 2);
    return 0;
}

TEST(utf8_substr_middle) {
    std::string result = utf8_substr(utf8_test_strings::chinese, 1, 2);
    CHECK(is_valid_utf8(result));
    CHECK_EQ(utf8_char_count(result), 2);
    return 0;
}

TEST(utf8_substr_entire) {
    std::string result = utf8_substr(utf8_test_strings::chinese, 0, 4);
    CHECK_EQ(result, utf8_test_strings::chinese);
    return 0;
}

TEST(utf8_substr_ascii) {
    std::string result = utf8_substr(utf8_test_strings::ascii_hello, 0, 5);
    CHECK_EQ(result, "Hello");
    return 0;
}

// ============================================================================
// UTF-8 Character Boundary Tests
// ============================================================================

TEST(utf8_last_char_boundary) {
    size_t boundary = utf8_find_last_char_boundary(utf8_test_strings::chinese);
    CHECK_EQ(boundary, 9);
    return 0;
}

TEST(utf8_last_char_boundary_empty) {
    size_t boundary = utf8_find_last_char_boundary(utf8_test_strings::empty);
    CHECK_EQ(boundary, 0);
    return 0;
}

TEST(utf8_last_char_boundary_ascii) {
    size_t boundary = utf8_find_last_char_boundary(utf8_test_strings::ascii_hello);
    CHECK_EQ(boundary, utf8_test_strings::ascii_hello.size() - 1);
    return 0;
}

// ============================================================================
// UTF-8 Continuation Byte Tests
// ============================================================================

TEST(utf8_continuation_valid) {
    for (int i = 0x80; i <= 0xBF; ++i) {
        CHECK(is_utf8_continuation_byte(static_cast<uint8_t>(i)));
    }
    return 0;
}

TEST(utf8_continuation_invalid) {
    for (int i = 0; i <= 0x7F; ++i) {
        CHECK_FALSE(is_utf8_continuation_byte(static_cast<uint8_t>(i)));
    }
    return 0;
}

// ============================================================================
// UTF-8 Special Cases
// ============================================================================

TEST(utf8_whitespace) {
    CHECK(is_valid_utf8(utf8_test_strings::newlines));
    CHECK(is_valid_utf8(utf8_test_strings::tabs));
    CHECK(is_valid_utf8(utf8_test_strings::non_breaking_space));
    return 0;
}

TEST(utf8_non_breaking_space) {
    std::string nbsp = "\u00A0";
    CHECK(is_valid_utf8(nbsp));
    CHECK_EQ(utf8_char_count(nbsp), 1);
    CHECK_EQ(utf8_char_length(nbsp, 0), 2);
    return 0;
}

TEST(utf8_zero_width) {
    std::string zwnbsp = "\uFEFF";
    CHECK(is_valid_utf8(zwnbsp));
    CHECK_EQ(utf8_char_count(zwnbsp), 1);
    return 0;
}

TEST(utf8_sanitize_replace_invalid_drops_stray_continuations) {
    // §fix (2026-07-18): replacing an invalid sequence start with '?'
    // must also drop the partial continuation bytes already appended
    // after it — otherwise the output still contains stray 0x80-0xBF
    // bytes and remains invalid UTF-8.

    // 3-byte lead + 1 valid continuation, then an invalid continuation
    std::string broken;
    broken += static_cast<char>(0xE4);
    broken += static_cast<char>(0xBD);
    broken += 'A';
    std::string cleaned = utf8_sanitize_replace_invalid(broken);
    CHECK_EQ(cleaned, std::string("?A"));
    CHECK(is_valid_utf8(cleaned));

    // Truncated sequence at end of input
    std::string truncated = "AB";
    truncated += static_cast<char>(0xE0);
    truncated += static_cast<char>(0x80);
    std::string cleaned2 = utf8_sanitize_replace_invalid(truncated);
    CHECK_EQ(cleaned2, std::string("AB?"));
    CHECK(is_valid_utf8(cleaned2));

    // Valid input passes through unchanged
    CHECK_EQ(utf8_sanitize_replace_invalid(utf8_test_strings::chinese),
             utf8_test_strings::chinese);

    return 0;
}

// ============================================================================
// Main - Test Runner
// ============================================================================

int main() {
    int failures = 0;
    
    std::cout << "Running UTF-8 validation tests..." << std::endl;
    
    // Validation tests
    failures += test_utf8_valid_ascii();
    failures += test_utf8_valid_multi_byte();
    failures += test_utf8_invalid_sequences();
    failures += test_utf8_edge_cases();
    
    // Character length tests
    failures += test_utf8_char_length_ascii();
    failures += test_utf8_char_length_extended();
    failures += test_utf8_char_length_cjk();
    failures += test_utf8_char_length_emoji();
    failures += test_utf8_char_length_helpers();
    
    // Character count tests
    failures += test_utf8_char_count_ascii();
    failures += test_utf8_char_count_multi_byte();
    failures += test_utf8_char_count_mixed();
    
    // Truncation tests
    failures += test_utf8_truncate_smaller();
    failures += test_utf8_truncate_ascii_boundary();
    failures += test_utf8_truncate_chinese_boundary();
    failures += test_utf8_truncate_emoji_boundary();
    failures += test_utf8_truncate_zero();
    failures += test_utf8_truncate_original_size();
    failures += test_utf8_truncate_larger_size();
    failures += test_utf8_truncate_chars_chinese();
    failures += test_utf8_truncate_chars_ascii();
    failures += test_utf8_truncate_chars_emoji();
    
    // Index conversion tests
    failures += test_utf8_byte_to_char_ascii();
    failures += test_utf8_byte_to_char_multi_byte();
    failures += test_utf8_char_to_byte_ascii();
    failures += test_utf8_char_to_byte_multi_byte();
    
    // Substring tests
    failures += test_utf8_substr_basic();
    failures += test_utf8_substr_middle();
    failures += test_utf8_substr_entire();
    failures += test_utf8_substr_ascii();
    
    // Character boundary tests
    failures += test_utf8_last_char_boundary();
    failures += test_utf8_last_char_boundary_empty();
    failures += test_utf8_last_char_boundary_ascii();
    
    // Continuation byte tests
    failures += test_utf8_continuation_valid();
    failures += test_utf8_continuation_invalid();
    
    // Special cases
    failures += test_utf8_whitespace();
    failures += test_utf8_non_breaking_space();
    failures += test_utf8_zero_width();
    failures += test_utf8_sanitize_replace_invalid_drops_stray_continuations();
    
    std::cout << "\nResults: " << (failures == 0 ? "PASS" : "FAIL") << std::endl;
    std::cout << "Failed: " << failures << " tests" << std::endl;
    
    return failures > 0 ? 1 : 0;
}
