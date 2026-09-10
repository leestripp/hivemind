/**
 * @file test_string_utils.cpp
 * @brief Unit tests for string utility functions
 * 
 * Uses CMake/CTest for test discovery - no external dependencies.
 */

#include <iostream>
#include <string>
#include <sstream>
#include <cstdlib>
#include "core/Utf8Utils.h"

// ============================================================================
// Test Registration
// ============================================================================
#define TEST(name) int test_##name()

// Helper macro for assertions
#define CHECK_EQ(a, b) do { \
    if ((a) != (b)) { \
        std::cerr << "FAIL: " << #a << " == " << #b << " (" << (a) << " vs " << (b) << ")" << std::endl; \
        return 1; \
    } \
} while(0)

#define CHECK_NE(a, b) do { \
    if ((a) == (b)) { \
        std::cerr << "FAIL: " << #a << " != " << #b << " (both equal to " << (a) << ")" << std::endl; \
        return 1; \
    } \
} while(0)

#define CHECK(cond) do { \
    if (!(cond)) { \
        std::cerr << "FAIL: " << #cond << std::endl; \
        return 1; \
    } \
} while(0)

// ============================================================================
// Helper Functions (duplicated from source for standalone testing)
// ============================================================================

static std::string trim(const std::string& str) {
    size_t start = 0;
    while (start < str.length() && (str[start] == ' ' || str[start] == '\t' || str[start] == '\n' || str[start] == '\r')) {
        start++;
    }
    
    size_t end = str.length();
    while (end > start && (str[end - 1] == ' ' || str[end - 1] == '\t' || str[end - 1] == '\n' || str[end - 1] == '\r')) {
        end--;
    }
    
    return str.substr(start, end - start);
}

static std::string truncate_for_display(const std::string& str, size_t max_len) {
    if (str.length() <= max_len) return str;
    if (max_len < 3) return "";
    
    // Try UTF-8 aware truncation first
    if (is_valid_utf8(str)) {
        return utf8_truncate(str, max_len - 3) + "...";
    }
    
    // Fallback for non-UTF-8 content
    return str.substr(0, max_len - 3) + "...";
}

// ============================================================================
// Trim Function Tests
// ============================================================================

TEST(trim_whitespace_both_ends) {
    CHECK_EQ(trim("  hello  "), "hello");
    CHECK_EQ(trim("\thello\t"), "hello");
    CHECK_EQ(trim("\nhello\n"), "hello");
    CHECK_EQ(trim("\rhello\r"), "hello");
    return 0;
}

TEST(trim_leading_whitespace) {
    CHECK_EQ(trim("  hello"), "hello");
    CHECK_EQ(trim("\t\thello"), "hello");
    CHECK_EQ(trim("\n\nhello"), "hello");
    return 0;
}

TEST(trim_trailing_whitespace) {
    CHECK_EQ(trim("hello  "), "hello");
    CHECK_EQ(trim("hello\t\t"), "hello");
    CHECK_EQ(trim("hello\n\n"), "hello");
    return 0;
}

TEST(trim_no_whitespace) {
    CHECK_EQ(trim("hello"), "hello");
    CHECK_EQ(trim("hello world"), "hello world");
    return 0;
}

TEST(trim_only_whitespace) {
    CHECK_EQ(trim("   "), "");
    CHECK_EQ(trim("\t\t"), "");
    CHECK_EQ(trim("\n\n\n"), "");
    CHECK_EQ(trim("  \t  \n  "), "");
    return 0;
}

TEST(trim_empty_string) {
    CHECK_EQ(trim(""), "");
    return 0;
}

TEST(trim_mixed_whitespace) {
    CHECK_EQ(trim(" \t\n\r hello \t\n\r "), "hello");
    return 0;
}

TEST(trim_utf8_preserved) {
    std::string result = trim("  你好世界  ");
    CHECK_EQ(result, "你好世界");
    CHECK(is_valid_utf8(result));
    return 0;
}

TEST(trim_emoji_preserved) {
    std::string result = trim("  👋🌍  ");
    CHECK_EQ(result, "👋🌍");
    CHECK(is_valid_utf8(result));
    return 0;
}

// ============================================================================
// Truncate for Display Tests
// ============================================================================

TEST(truncate_shorter_than_max) {
    std::string result = truncate_for_display("hello", 100);
    CHECK_EQ(result, "hello");
    return 0;
}

TEST(truncate_equal_to_max) {
    std::string result = truncate_for_display("hello", 5);
    CHECK_EQ(result, "hello");
    return 0;
}

TEST(truncate_longer_than_max) {
    std::string result = truncate_for_display("hello world", 8);
    CHECK(result.size() <= 8);
    CHECK(result.size() >= 5);
    return 0;
}

TEST(truncate_max_size_3) {
    std::string result = truncate_for_display("hello", 3);
    CHECK_EQ(result, "...");
    return 0;
}

TEST(truncate_max_size_4) {
    std::string result = truncate_for_display("hello", 4);
    CHECK(result.size() <= 4);
    CHECK(result.size() >= 3);
    return 0;
}

TEST(truncate_empty_string) {
    std::string result = truncate_for_display("", 10);
    CHECK_EQ(result, "");
    return 0;
}

TEST(truncate_max_less_than_3) {
    std::string result = truncate_for_display("hello", 2);
    CHECK_EQ(result, "");
    return 0;
}

TEST(truncate_utf8_no_splitting) {
    std::string result = truncate_for_display("你好世界", 6);
    CHECK(result.size() <= 9);
    CHECK(is_valid_utf8(result));
    bool ends_with_ellipsis = (result.size() >= 3 && result.substr(result.size() - 3) == "...");
    CHECK(ends_with_ellipsis);
    return 0;
}

// ============================================================================
// UTF-8 Aware Truncation Tests
// ============================================================================

TEST(utf8_truncate_ascii) {
    std::string result = utf8_truncate("hello world", 8);
    CHECK_EQ(result, "hello wo");
    CHECK(is_valid_utf8(result));
    return 0;
}

TEST(utf8_truncate_chinese) {
    std::string chinese = "你好世界";
    
    std::string result4 = utf8_truncate(chinese, 4);
    CHECK(is_valid_utf8(result4));
    CHECK(result4.size() <= 4);
    
    std::string result6 = utf8_truncate(chinese, 6);
    CHECK(is_valid_utf8(result6));
    CHECK_EQ(result6.size(), 6);
    CHECK_EQ(utf8_char_count(result6), 2);
    return 0;
}

TEST(utf8_truncate_emoji) {
    std::string emoji = "👋🌍🚀";
    
    std::string result4 = utf8_truncate(emoji, 4);
    CHECK(is_valid_utf8(result4));
    CHECK_EQ(result4.size(), 4);
    CHECK_EQ(utf8_char_count(result4), 1);
    
    std::string result8 = utf8_truncate(emoji, 8);
    CHECK(is_valid_utf8(result8));
    CHECK_EQ(result8.size(), 8);
    CHECK_EQ(utf8_char_count(result8), 2);
    return 0;
}

TEST(utf8_truncate_mixed) {
    std::string mixed = "Hello 世界!";
    
    std::string result = utf8_truncate(mixed, 8);
    CHECK(is_valid_utf8(result));
    CHECK(result.size() <= 8);
    
    result = utf8_truncate(mixed, 12);
    CHECK(is_valid_utf8(result));
    CHECK(result.size() <= 12);
    return 0;
}

TEST(utf8_truncate_no_need) {
    std::string result = utf8_truncate("hello", 10);
    CHECK_EQ(result, "hello");
    return 0;
}

TEST(utf8_truncate_zero) {
    std::string result = utf8_truncate("hello", 0);
    CHECK_EQ(result, "");
    return 0;
}

// ============================================================================
// String Stream Tests
// ============================================================================

TEST(stringstream_ascii) {
    std::stringstream ss;
    ss << "Hello" << " " << "World";
    std::string result = ss.str();
    CHECK_EQ(result, "Hello World");
    return 0;
}

TEST(stringstream_utf8) {
    std::stringstream ss;
    ss << "你好" << " " << "世界";
    std::string result = ss.str();
    CHECK_EQ(result, "你好 世界");
    CHECK(is_valid_utf8(result));
    return 0;
}

TEST(stringstream_numbers) {
    std::stringstream ss;
    ss << "Value: " << 42 << ", Float: " << 3.14159;
    std::string result = ss.str();
    CHECK_EQ(result, "Value: 42, Float: 3.14159");
    return 0;
}

TEST(stringstream_clearing) {
    std::stringstream ss;
    ss << "First";
    CHECK_EQ(ss.str(), "First");
    
    ss.str("");
    ss << "Second";
    CHECK_EQ(ss.str(), "Second");
    return 0;
}

// ============================================================================
// String Comparison Tests
// ============================================================================

TEST(ascii_comparison) {
    CHECK_EQ(trim("  hello  "), "hello");
    CHECK_EQ(trim("hello"), "hello");
    CHECK_NE(trim("hello"), "world");
    return 0;
}

TEST(utf8_string_equality) {
    std::string str1 = "你好世界";
    std::string str2 = "你好世界";
    std::string str3 = "你好";
    
    CHECK_EQ(str1, str2);
    CHECK_NE(str1, str3);
    CHECK_EQ(utf8_char_count(str1), utf8_char_count(str2));
    return 0;
}

// ============================================================================
// String Modification Tests
// ============================================================================

TEST(substring_extraction) {
    std::string str = "Hello, World!";
    CHECK_EQ(str.substr(0, 5), "Hello");
    CHECK_EQ(str.substr(7, 5), "World");
    CHECK_EQ(str.substr(7), "World!");
    return 0;
}

TEST(utf8_substr) {
    std::string chinese = "你好世界";
    
    std::string result = utf8_substr(chinese, 0, 2);
    CHECK(is_valid_utf8(result));
    CHECK_EQ(utf8_char_count(result), 2);
    
    result = utf8_substr(chinese, 1, 2);
    CHECK(is_valid_utf8(result));
    CHECK_EQ(utf8_char_count(result), 2);
    return 0;
}

TEST(string_append) {
    std::string str = "Hello";
    str += ", ";
    str += "World";
    CHECK_EQ(str, "Hello, World");
    return 0;
}

TEST(utf8_append) {
    std::string str = "Hello";
    str += " 世界";
    CHECK_EQ(str, "Hello 世界");
    CHECK(is_valid_utf8(str));
    return 0;
}

TEST(string_replace) {
    std::string str = "Hello World";
    size_t pos = str.find("World");
    if (pos != std::string::npos) {
        str.replace(pos, 5, "Universe");
    }
    CHECK_EQ(str, "Hello Universe");
    return 0;
}

// ============================================================================
// String Search Tests
// ============================================================================

TEST(find_in_ascii) {
    std::string str = "Hello, World!";
    CHECK_EQ(str.find("World"), 7);
    CHECK_EQ(str.find("Hello"), 0);
    CHECK_EQ(str.find("!"), 12);
    CHECK_EQ(str.find("?"), std::string::npos);
    return 0;
}

TEST(find_in_utf8) {
    std::string chinese = "你好世界";
    CHECK_EQ(chinese.find("你好"), 0);
    CHECK_EQ(chinese.find("世界"), 6);
    CHECK_EQ(chinese.find("他"), std::string::npos);
    return 0;
}

TEST(rfind_reverse) {
    std::string str = "Hello, World! World";
    CHECK_EQ(str.rfind("World"), 14);
    return 0;
}

TEST(find_empty_string) {
    std::string str = "Hello";
    CHECK_EQ(str.find(""), 0);
    return 0;
}

// ============================================================================
// String Length Tests
// ============================================================================

TEST(string_length) {
    CHECK_EQ(std::string("hello").length(), 5);
    CHECK_EQ(std::string("你好").length(), 6);
    CHECK_EQ(utf8_char_count("你好"), 2);
    return 0;
}

TEST(empty_string_length) {
    CHECK_EQ(std::string().length(), 0);
    CHECK_EQ(utf8_char_count(""), 0);
    return 0;
}

// ============================================================================
// Main - Test Runner
// ============================================================================

int main() {
    int failures = 0;
    
    std::cout << "Running string utility tests..." << std::endl;
    
    // Trim tests
    failures += test_trim_whitespace_both_ends();
    failures += test_trim_leading_whitespace();
    failures += test_trim_trailing_whitespace();
    failures += test_trim_no_whitespace();
    failures += test_trim_only_whitespace();
    failures += test_trim_empty_string();
    failures += test_trim_mixed_whitespace();
    failures += test_trim_utf8_preserved();
    failures += test_trim_emoji_preserved();
    
    // Truncate tests
    failures += test_truncate_shorter_than_max();
    failures += test_truncate_equal_to_max();
    failures += test_truncate_longer_than_max();
    failures += test_truncate_max_size_3();
    failures += test_truncate_max_size_4();
    failures += test_truncate_empty_string();
    failures += test_truncate_max_less_than_3();
    failures += test_truncate_utf8_no_splitting();
    
    // UTF-8 truncate tests
    failures += test_utf8_truncate_ascii();
    failures += test_utf8_truncate_chinese();
    failures += test_utf8_truncate_emoji();
    failures += test_utf8_truncate_mixed();
    failures += test_utf8_truncate_no_need();
    failures += test_utf8_truncate_zero();
    
    // StringStream tests
    failures += test_stringstream_ascii();
    failures += test_stringstream_utf8();
    failures += test_stringstream_numbers();
    failures += test_stringstream_clearing();
    
    // Comparison tests
    failures += test_ascii_comparison();
    failures += test_utf8_string_equality();
    
    // Modification tests
    failures += test_substring_extraction();
    failures += test_utf8_substr();
    failures += test_string_append();
    failures += test_utf8_append();
    failures += test_string_replace();
    
    // Search tests
    failures += test_find_in_ascii();
    failures += test_find_in_utf8();
    failures += test_rfind_reverse();
    failures += test_find_empty_string();
    
    // Length tests
    failures += test_string_length();
    failures += test_empty_string_length();
    
    std::cout << "\nResults: " << (failures == 0 ? "PASS" : "FAIL") << std::endl;
    std::cout << "Failed: " << failures << " tests" << std::endl;
    
    return failures > 0 ? 1 : 0;
}