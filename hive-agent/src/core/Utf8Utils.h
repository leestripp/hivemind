#pragma once

#include <string>
#include <cstdint>
#include <cstddef>

/**
 * @file Utf8Utils.h
 * @brief UTF-8 encoding utilities for safe string handling
 * 
 * This module provides utilities for validating and manipulating UTF-8 encoded strings.
 * UTF-8 uses 1-4 bytes per character:
 *   - 1 byte: 0xxxxxxx (ASCII range 0x00-0x7F)
 *   - 2 bytes: 110xxxxx 10xxxxxx (0xC0-0xDF, 0x80-0xBF)
 *   - 3 bytes: 1110xxxx 10xxxxxx 10xxxxxx (0xE0-0xEF, 0x80-0xBF)
 *   - 4 bytes: 11110xxx 10xxxxxx 10xxxxxx 10xxxxxx (0xF0-0xF7, 0x80-0xBF)
 */

/**
 * @brief Check if a byte is a UTF-8 continuation byte
 * @param byte The byte to check
 * @return true if byte is a continuation byte (0x80-0xBF)
 */
inline bool is_utf8_continuation_byte(uint8_t byte) {
    return (byte & 0xC0) == 0x80;
}

/**
 * @brief Get the expected byte length of a UTF-8 character from its first byte
 * @param first_byte The first byte of a UTF-8 character
 * @return The total byte length (1-4), or 0 if invalid start byte
 */
inline size_t utf8_char_length_from_first_byte(uint8_t first_byte) {
    if ((first_byte & 0x80) == 0) {
        return 1;  // ASCII
    } else if ((first_byte & 0xE0) == 0xC0) {
        return 2;  // 2-byte sequence
    } else if ((first_byte & 0xF0) == 0xE0) {
        return 3;  // 3-byte sequence
    } else if ((first_byte & 0xF8) == 0xF0) {
        return 4;  // 4-byte sequence
    }
    return 0;  // Invalid start byte
}

/**
 * @brief Check if a byte sequence starting at pos is a valid UTF-8 character
 * @param data Pointer to the byte data
 * @param pos Current position in the data
 * @param len Total length of the data
 * @return true if valid UTF-8 character found, false otherwise
 */
inline bool is_valid_utf8_char(const char* data, size_t pos, size_t len) {
    if (pos >= len) return false;
    
    uint8_t first_byte = static_cast<uint8_t>(data[pos]);
    size_t char_len = utf8_char_length_from_first_byte(first_byte);
    
    if (char_len == 0) return false;
    if (pos + char_len > len) return false;
    
    // Validate continuation bytes
    for (size_t i = 1; i < char_len; ++i) {
        if (!is_utf8_continuation_byte(static_cast<uint8_t>(data[pos + i]))) {
            return false;
        }
    }
    
    // Additional validation for overlong encodings and invalid code points
    if (char_len == 2) {
        // Check for overlong encoding: code point should be >= 0x80
        uint32_t codepoint = ((first_byte & 0x1F) << 6) | (data[pos + 1] & 0x3F);
        if (codepoint < 0x80) return false;
    } else if (char_len == 3) {
        uint32_t codepoint = ((first_byte & 0x0F) << 12) |
                             ((data[pos + 1] & 0x3F) << 6) |
                             (data[pos + 2] & 0x3F);
        // Check for overlong encoding and invalid code points (surrogates)
        if (codepoint < 0x800 || (codepoint >= 0xD800 && codepoint <= 0xDFFF)) {
            return false;
        }
    } else if (char_len == 4) {
        uint32_t codepoint = ((first_byte & 0x07) << 18) |
                             ((data[pos + 1] & 0x3F) << 12) |
                             ((data[pos + 2] & 0x3F) << 6) |
                             (data[pos + 3] & 0x3F);
        // Check for overlong encoding and invalid code points (> U+10FFFF)
        if (codepoint < 0x10000 || codepoint > 0x10FFFF) {
            return false;
        }
    }
    
    return true;
}

/**
 * @brief Validate if a string contains valid UTF-8 encoding
 * @param str The string to validate
 * @return true if the entire string is valid UTF-8, false otherwise
 */
inline bool is_valid_utf8(const std::string& str) {
    const char* data = str.data();
    size_t len = str.size();
    size_t pos = 0;
    
    while (pos < len) {
        if (!is_valid_utf8_char(data, pos, len)) {
            return false;
        }
        pos += utf8_char_length_from_first_byte(static_cast<uint8_t>(data[pos]));
    }
    
    return true;
}

/**
 * @brief Get the number of bytes in a UTF-8 character starting at the given position
 * @param str The string containing UTF-8 data
 * @param pos Position of the first byte of the character
 * @return Number of bytes in the character (1-4), or 0 if invalid
 */
inline size_t utf8_char_length(const std::string& str, size_t pos) {
    if (pos >= str.size()) return 0;
    return utf8_char_length_from_first_byte(static_cast<uint8_t>(str[pos]));
}

/**
 * @brief Count the number of Unicode characters (code points) in a UTF-8 string
 * @param str The UTF-8 string
 * @return Number of Unicode characters
 */
inline size_t utf8_char_count(const std::string& str) {
    size_t count = 0;
    size_t pos = 0;
    const size_t len = str.size();
    
    while (pos < len) {
        size_t char_len = utf8_char_length(str, pos);
        if (char_len == 0) {
            // Invalid byte, count as 1 byte
            ++pos;
        } else {
            pos += char_len;
            ++count;
        }
    }
    
    return count;
}

/**
 * @brief Truncate a UTF-8 string to a maximum byte length, preserving character boundaries
 * @param str The string to truncate
 * @param max_bytes Maximum number of bytes in the result
 * @return Truncated string that ends on a valid character boundary
 */
inline std::string utf8_truncate(const std::string& str, size_t max_bytes) {
    if (str.size() <= max_bytes) {
        return str;
    }
    
    if (max_bytes == 0) {
        return "";
    }
    
    const char* data = str.data();
    size_t pos = 0;
    
    while (pos < max_bytes) {
        size_t char_len = utf8_char_length_from_first_byte(static_cast<uint8_t>(data[pos]));
        
        if (char_len == 0) {
            // Invalid byte, stop here
            break;
        }
        
        if (pos + char_len > max_bytes) {
            // Would exceed limit, stop here
            break;
        }
        
        pos += char_len;
    }
    
    return str.substr(0, pos);
}

/**
 * @brief Truncate a UTF-8 string to a maximum character count
 * @param str The string to truncate
 * @param max_chars Maximum number of characters in the result
 * @return Truncated string with at most max_chars characters
 */
inline std::string utf8_truncate_chars(const std::string& str, size_t max_chars) {
    size_t count = 0;
    size_t pos = 0;
    const size_t len = str.size();
    
    while (pos < len && count < max_chars) {
        size_t char_len = utf8_char_length(str, pos);
        
        if (char_len == 0) {
            // Invalid byte, skip it
            ++pos;
        } else {
            pos += char_len;
            ++count;
        }
    }
    
    return str.substr(0, pos);
}

/**
 * @brief Convert a byte offset to a character (code point) offset
 * @param str The UTF-8 string
 * @param byte_offset The byte offset
 * @return The corresponding character offset
 */
inline size_t utf8_byte_to_char_index(const std::string& str, size_t byte_offset) {
    size_t char_index = 0;
    size_t pos = 0;
    const size_t len = str.size();
    
    while (pos < len && pos < byte_offset) {
        size_t char_len = utf8_char_length(str, pos);
        if (char_len == 0) {
            // Invalid byte, count as 1
            ++pos;
            ++char_index;
        } else {
            pos += char_len;
            ++char_index;
        }
    }
    
    // If we stopped mid-character, we've already counted that character
    return char_index;
}

/**
 * @brief Convert a character (code point) offset to a byte offset
 * @param str The UTF-8 string
 * @param char_offset The character offset
 * @return The corresponding byte offset, or string size if beyond end
 */
inline size_t utf8_char_to_byte_index(const std::string& str, size_t char_offset) {
    size_t count = 0;
    size_t pos = 0;
    const size_t len = str.size();
    
    while (pos < len && count < char_offset) {
        size_t char_len = utf8_char_length(str, pos);
        if (char_len == 0) {
            ++pos;
        } else {
            pos += char_len;
            ++count;
        }
    }
    
    return pos;
}

/**
 * @brief Get the last complete UTF-8 character in a string
 * @param str The UTF-8 string
 * @return Position of the last complete character, or 0 if string is empty
 */
inline size_t utf8_find_last_char_boundary(const std::string& str) {
    if (str.empty()) return 0;
    
    size_t pos = str.size() - 1;
    
    // Scan backwards to find the start of the last character
    while (pos > 0 && is_utf8_continuation_byte(static_cast<uint8_t>(str[pos]))) {
        --pos;
    }
    
    // Verify this is actually a valid character start
    size_t char_len = utf8_char_length(str, pos);
    if (char_len == 0 || pos + char_len > str.size()) {
        // Not a valid character start, return position of last valid boundary
        return pos;
    }
    
    return pos;
}

/**
 * @brief Safely get a substring by character index, not byte index
 * @param str The UTF-8 string
 * @param char_start Starting character index
 * @param char_count Number of characters to extract
 * @return The substring
 */
inline std::string utf8_substr(const std::string& str, size_t char_start, size_t char_count) {
    size_t byte_start = utf8_char_to_byte_index(str, char_start);
    size_t byte_end = utf8_char_to_byte_index(str, char_start + char_count);
    return str.substr(byte_start, byte_end - byte_start);
}

/**
 * @brief Check if a string contains only ASCII characters
 * @param str The string to check
 * @return true if all bytes are in ASCII range (0x00-0x7F)
 */
inline bool is_ascii_only(const std::string& str) {
    for (unsigned char c : str) {
        if (c > 127) return false;
    }
    return true;
}

/**
 * @brief Common UTF-8 test strings for testing
 */
/**
 * @brief Replace invalid UTF-8 bytes with '?' using a state-machine.
 * Correctly handles multi-byte sequences: replaces an invalid sequence
 * (start byte plus any partial continuations) with a single '?', then
 * re-evaluates the offending byte as a fresh sequence start.
 * @param input The string to sanitize
 * @return A new string with invalid bytes replaced by '?'
 */
/**
 * @brief Truncate to at most max_bytes WITHOUT splitting a multi-byte
 * UTF-8 codepoint. Byte-level truncation (resize/substr) can cut a
 * 2-4 byte sequence in half, producing invalid UTF-8 that later kills
 * nlohmann::json dump/parse with type_error.316 (§BUG-UTF8-TRUNC).
 * Backs the cut point off to the previous codepoint boundary.
 */
inline std::string utf8_truncate_safe(const std::string& input, size_t max_bytes) {
    if (input.size() <= max_bytes) return input;
    size_t cut = max_bytes;
    // While the byte at the cut is a continuation byte (10xxxxxx), the
    // codepoint started earlier — back off to its start.
    while (cut > 0 && is_utf8_continuation_byte(static_cast<uint8_t>(input[cut])))
        --cut;
    return input.substr(0, cut);
}

inline std::string utf8_sanitize_replace_invalid(const std::string& input) {
    int expected_continuations = 0;
    size_t sequence_start_pos = 0;
    std::string result;
    result.reserve(input.size());

    for (unsigned char c : input) {
        if (expected_continuations > 0) {
            if ((c >= 0x80 && c <= 0xBF)) {
                // Valid continuation byte
                result += static_cast<char>(c);
                expected_continuations--;
            } else {
                // Invalid continuation byte: replace the sequence start with '?'
                result[sequence_start_pos] = '?';
                // §fix (2026-07-18): drop the partial continuation bytes
                // already appended after the sequence start — they are
                // stray 0x80-0xBF bytes and would leave the output
                // invalid UTF-8. (Supersedes the §B12 note: appended
                // continuations are NOT re-evaluated by the loop; only
                // the current byte is.)
                result.resize(sequence_start_pos + 1);
                expected_continuations = 0;
                // Re-evaluate current byte as a fresh start
                if (c < 0x80) {
                    result += static_cast<char>(c);
                } else if (c >= 0xC0 && c <= 0xDF) {
                    sequence_start_pos = result.size();
                    result += static_cast<char>(c);
                    expected_continuations = 1;
                } else if (c >= 0xE0 && c <= 0xEF) {
                    sequence_start_pos = result.size();
                    result += static_cast<char>(c);
                    expected_continuations = 2;
                } else if (c >= 0xF0 && c <= 0xF7) {
                    sequence_start_pos = result.size();
                    result += static_cast<char>(c);
                    expected_continuations = 3;
                } else {
                    result += '?';
                }
            }
        } else {
            if (c < 0x80) {
                result += static_cast<char>(c);
            } else if (c >= 0xC0 && c <= 0xDF) {
                sequence_start_pos = result.size();
                result += static_cast<char>(c);
                expected_continuations = 1;
            } else if (c >= 0xE0 && c <= 0xEF) {
                sequence_start_pos = result.size();
                result += static_cast<char>(c);
                expected_continuations = 2;
            } else if (c >= 0xF0 && c <= 0xF7) {
                sequence_start_pos = result.size();
                result += static_cast<char>(c);
                expected_continuations = 3;
            } else {
                result += '?';
            }
        }
    }

    // Handle trailing incomplete sequences at end of input
    if (expected_continuations > 0) {
        result[sequence_start_pos] = '?';
        // §fix (2026-07-18): drop the partial continuations too (same
        // reasoning as the invalid-continuation branch above).
        result.resize(sequence_start_pos + 1);
    }

    return result;
}

namespace utf8_test_strings {
    // ASCII strings
    inline const std::string empty = "";
    inline const std::string ascii_hello = "Hello, World!";
    inline const std::string ascii_special = "!@#$%^&*()_+-=[]{}|;':\",./<>?";
    
    // Common multi-byte UTF-8
    inline const std::string chinese = "你好世界";  // Chinese: "Hello World"
    inline const std::string japanese = "こんにちは世界";  // Japanese: "Hello World"
    inline const std::string korean = "안녕하세요";  // Korean: "Hello"
    inline const std::string emoji = "👋🌍🚀🎉";  // Emoji
    inline const std::string mixed_ascii_utf8 = "Hello 世界! 🌍";
    
    // European languages
    inline const std::string french = "Bonjour le monde";  // French
    inline const std::string german = "Guten Tag Welt";  // German
    inline const std::string spanish = "Hola Mundo";  // Spanish
    inline const std::string russian = "Привет мир";  // Russian (Cyrillic)
    inline const std::string arabic = "مرحبا بالعالم";  // Arabic
    inline const std::string hebrew = "שלום עולם";  // Hebrew
    
    // Special characters
    inline const std::string newlines = "Line1\nLine2\r\nLine3";
    inline const std::string tabs = "Col1\tCol2\tCol3";
    inline const std::string non_breaking_space = "Word\u00A0Word";  // Non-breaking space
    
    // Edge cases
    inline const std::string four_byte_chars = "🎉🔥✨💻";  // 4-byte emoji
    inline const std::string long_string = "Lorem ipsum dolor sit amet, consectetur adipiscing elit. "
                                          "中文测试 한국어 العربية עברית";
}  // namespace utf8_test_strings
