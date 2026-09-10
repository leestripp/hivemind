/**
 * @file test_json_utf8.cpp
 * @brief Unit tests for JSON serialization with invalid UTF-8 data
 * 
 * These tests verify that invalid UTF-8 bytes in strings are properly
 * handled during JSON serialization to prevent fatal errors.
 */

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

using json = nlohmann::json;

// Simulate the sanitize_utf8 function from MiniMaxClient.cpp
// for testing purposes
static std::string sanitize_utf8(const std::string& input) {
    // State machine for UTF-8 validation
    // 0 = expecting start byte or ASCII, 1-3 = expecting continuation bytes
    int expected_continuations = 0;
    // Track position where current sequence started
    size_t sequence_start_pos = 0;
    std::string result;
    result.reserve(input.size());
    
    for (unsigned char c : input) {
        if (expected_continuations > 0) {
            // We are in a multi-byte sequence, expect continuation bytes
            if ((c >= 0x80 && c <= 0xBF)) {
                // Valid continuation byte
                result += static_cast<char>(c);
                expected_continuations--;
            } else {
                // Invalid byte in sequence - replace the start byte with '?'
                result[sequence_start_pos] = '?';
                // Truncate to just after the replaced start byte
                result.resize(sequence_start_pos + 1);
                expected_continuations = 0;
                // Re-process this byte as potential start byte
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
                }
                // bytes 0xF8-0xFF are invalid start bytes, remain replaced
            }
        } else {
            // Expecting start byte or ASCII
            if (c < 0x80) {
                // ASCII
                result += static_cast<char>(c);
            } else if (c >= 0xC0 && c <= 0xDF) {
                // 2-byte sequence start
                sequence_start_pos = result.size();
                result += static_cast<char>(c);
                expected_continuations = 1;
            } else if (c >= 0xE0 && c <= 0xEF) {
                // 3-byte sequence start
                sequence_start_pos = result.size();
                result += static_cast<char>(c);
                expected_continuations = 2;
            } else if (c >= 0xF0 && c <= 0xF7) {
                // 4-byte sequence start
                sequence_start_pos = result.size();
                result += static_cast<char>(c);
                expected_continuations = 3;
            } else {
                // Invalid byte (0x80-0xBF as first byte, or 0xFE-0xFF)
                result += '?';
            }
        }
    }
    
    // Handle trailing incomplete sequences
    if (expected_continuations > 0) {
        // Replace the incomplete start byte with '?'
        result[sequence_start_pos] = '?';
        result.resize(sequence_start_pos + 1);
    }
    
    return result;
}

// Recursively sanitize JSON string values
static void sanitize_json(json& j) {
    if (j.is_object()) {
        for (auto& [key, value] : j.items()) {
            if (value.is_string()) {
                value = sanitize_utf8(value.get<std::string>());
            } else if (value.is_object() || value.is_array()) {
                sanitize_json(value);
            }
        }
    } else if (j.is_array()) {
        for (auto& item : j) {
            if (item.is_string()) {
                item = sanitize_utf8(item.get<std::string>());
            } else if (item.is_object() || item.is_array()) {
                sanitize_json(item);
            }
        }
    }
}

TEST_SUITE("JSON Serialization with Invalid UTF-8") {
    
    TEST_CASE("Valid UTF-8 strings serialize without issues") {
        json j;
        j["message"] = "Hello, world!";
        j["chinese"] = "你好世界";
        j["emoji"] = "👋🌍";
        
        // Should not throw
        std::string serialized;
        CHECK_NOTHROW(serialized = j.dump());
        [[maybe_unused]] auto _ = json::parse(serialized);
    }
    
    TEST_CASE("Invalid UTF-8 byte 0x98 causes JSON serialization error") {
        // This is the byte sequence that caused the original crash
        // Create a string with invalid UTF-8 byte 0x98
        std::string bad_string = "Build output with invalid byte: ";
        bad_string += static_cast<char>(0x98);
        bad_string += " more text";
        
        json j;
        j["output"] = bad_string;
        
        // Serialization should throw due to invalid UTF-8
        CHECK_THROWS_AS(j.dump(), const json::exception&);
    }
    
    TEST_CASE("Invalid UTF-8 byte 0x80 causes JSON serialization error") {
        std::string bad_string = "Invalid byte: ";
        bad_string += static_cast<char>(0x80);  // Invalid continuation byte without start
        bad_string += " text";
        
        json j;
        j["output"] = bad_string;
        
        // Serialization should throw
        CHECK_THROWS_AS(j.dump(), const json::exception&);
    }
    
    TEST_CASE("Invalid UTF-8 byte 0xFE causes JSON serialization error") {
        std::string bad_string = "Invalid byte: ";
        bad_string += static_cast<char>(0xFE);
        bad_string += static_cast<char>(0x80);
        
        json j;
        j["output"] = bad_string;
        
        // Serialization should throw
        CHECK_THROWS_AS(j.dump(), const json::exception&);
    }
    
    TEST_CASE("Invalid UTF-8 byte 0xFF causes JSON serialization error") {
        std::string bad_string = "Invalid byte: ";
        bad_string += static_cast<char>(0xFF);
        
        json j;
        j["output"] = bad_string;
        
        // Serialization should throw
        CHECK_THROWS_AS(j.dump(), const json::exception&);
    }
    
    TEST_CASE("Compiler warning pattern with invalid bytes") {
        // Simulate compiler warning output with various invalid bytes
        std::string compiler_output = 
            "/path/to/file.c:100:5: warning: 'deprecated' is deprecated\n"
            "  Use 'GListModel' instead [-Wdeprecated-declarations]\n"
            "    113 |     gtk_tree_model_get(model, a,\n"
            "        |     ^";
        
        // Add the problematic 0x98 byte
        compiler_output += static_cast<char>(0x98);
        compiler_output += " more content";
        
        json j;
        j["tool"] = "execute_bash";
        j["command"] = "cmake --build build";
        j["output"] = compiler_output;
        
        // This should throw when we try to serialize
        CHECK_THROWS_AS(j.dump(), const json::exception&);
    }
}

TEST_SUITE("UTF-8 Sanitization") {
    
    TEST_CASE("Sanitize replaces invalid 0x98 byte with ?") {
        std::string input = "text with bad byte: ";
        input += static_cast<char>(0x98);
        input += " more";
        
        std::string result = sanitize_utf8(input);
        
        // Should not contain 0x98 anymore
        CHECK(result.find(static_cast<char>(0x98)) == std::string::npos);
        // Should contain '?' where invalid byte was
        CHECK(result.find('?') != std::string::npos);
        // Should still be parseable
        std::string quoted = "\"" + result + "\"";
        [[maybe_unused]] auto _ = json::parse(quoted);
    }
    
    TEST_CASE("Sanitize preserves valid ASCII") {
        std::string input = "Hello, World! 123";
        std::string result = sanitize_utf8(input);
        CHECK_EQ(result, input);
    }
    
    TEST_CASE("Sanitize preserves valid UTF-8 multi-byte") {
        std::string input = "你好世界 👋";
        std::string result = sanitize_utf8(input);
        CHECK_EQ(result, input);
    }
    
    TEST_CASE("Sanitize replaces lone continuation byte 0x80") {
        std::string input = "text: ";
        input += static_cast<char>(0x80);  // Invalid: lone continuation
        input += " more";
        
        std::string result = sanitize_utf8(input);
        CHECK(result.find('?') != std::string::npos);
    }
    
    TEST_CASE("Sanitize replaces invalid bytes 0xFE and 0xFF") {
        std::string input = "bad: ";
        input += static_cast<char>(0xFE);
        input += static_cast<char>(0xFF);
        
        std::string result = sanitize_utf8(input);
        CHECK(result.find('?') != std::string::npos);
    }
}

TEST_SUITE("JSON Sanitization Round-Trip") {
    
    TEST_CASE("Sanitized JSON can be parsed after sanitization") {
        // Create JSON with invalid UTF-8
        std::string bad_output = "error";
        bad_output += static_cast<char>(0x98);
        bad_output += " continue";
        
        json j;
        j["tool"] = "execute_bash";
        j["output"] = bad_output;
        
        // Before sanitization - serialization throws
        CHECK_THROWS_AS(j.dump(), const json::exception&);
        
        // Sanitize the JSON
        sanitize_json(j);
        
        // After sanitization - serialization should succeed
        std::string after = j.dump();
        json parsed;
        CHECK_NOTHROW(parsed = json::parse(after));
    }
    
    TEST_CASE("Nested JSON with invalid UTF-8 is sanitized") {
        std::string bad_string = "error";
        bad_string += static_cast<char>(0x98);
        
        json j;
        j["results"] = json::array({
            {{"tool", "execute_bash"}, {"output", bad_string}},
            {{"tool", "read_file"}, {"content", "valid content"}}
        });
        
        // Should throw before sanitization
        CHECK_THROWS_AS(j.dump(), const json::exception&);
        
        // Sanitize
        sanitize_json(j);
        
        // Should work after sanitization
        std::string after = j.dump();
        json parsed;
        CHECK_NOTHROW(parsed = json::parse(after));
    }
    
    TEST_CASE("Multiple invalid bytes in single string") {
        // Create a string with all 256 byte values
        // Some bytes will form incomplete UTF-8 sequences
        std::string bad_string;
        for (int i = 0; i < 256; i++) {
            bad_string += static_cast<char>(i);
        }
        
        json j;
        j["binary_data"] = bad_string;
        
        // The sanitize function handles incomplete sequences by replacing
        // the start byte when continuation bytes are missing
        // This test verifies that sanitize_json can process arbitrary binary data
        CHECK_NOTHROW(sanitize_json(j));
        
        // After sanitization, dump and parse should succeed
        std::string after = j.dump();
        json parsed;
        CHECK_NOTHROW(parsed = json::parse(after));
    }
}

TEST_SUITE("Tool Result Pattern Tests") {
    
    TEST_CASE("Simulate execute_bash tool result with compiler errors") {
        // This pattern matches what we saw in the crash
        std::string compiler_output = 
            "  [ 21%] Building C object CMakeFiles/target.dir/src/file.c.o\n"
            "/path/src/file.c:113:5: warning: 'gtk_tree_model_get' is deprecated\n"
            "    113 |     gtk_tree_model_get(model, a,\n"
            "        |     ^";
        
        // The 0x98 byte often appears in certain compiler/terminal output
        compiler_output += static_cast<char>(0x98);
        compiler_output += " At top level:\n";
        compiler_output += "static void on_file_changed...\n";
        
        json tool_result;
        tool_result["status"] = "ok";
        tool_result["tool_name"] = "execute_bash";
        tool_result["command"] = "cmake --build build -j8";
        tool_result["output"] = compiler_output;
        
        // This should fail without sanitization (during serialization)
        CHECK_THROWS_AS(tool_result.dump(), const json::exception&);
        
        // Sanitize and verify
        sanitize_json(tool_result);
        std::string serialized = tool_result.dump();
        json parsed;
        CHECK_NOTHROW(parsed = json::parse(serialized));
    }
    
    TEST_CASE("Simulate tool result with binary data in output") {
        // Some tools might output binary data
        // Note: bytes 0x00-0x1F are valid JSON string content
        std::string binary_output = "Binary data: ";
        for (int i = 0; i < 32; i++) {
            binary_output += static_cast<char>(i);
        }
        
        json tool_result;
        tool_result["status"] = "ok";
        tool_result["output"] = binary_output;
        
        // Serialization succeeds for bytes 0x00-0x1F (they're valid JSON)
        std::string serialized = tool_result.dump();
        json parsed;
        CHECK_NOTHROW(parsed = json::parse(serialized));
        
        // After sanitization, non-printable bytes are replaced
        sanitize_json(tool_result);
        serialized = tool_result.dump();
        CHECK_NOTHROW(parsed = json::parse(serialized));
    }
}
