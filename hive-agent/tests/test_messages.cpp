/**
 * @file test_messages.cpp
 * @brief Unit tests for Messages class - split and replace functions
 * 
 * Tests message splitting logic and replacement functionality.
 * Uses standalone approach to avoid external dependencies.
 */

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <string>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <cstdio>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <signal.h>
#include <cstdlib>

// Include Messages.h after doctest so FRIEND_TEST is available
#include "core/Messages.h"

using json = nlohmann::json;
namespace fs = std::filesystem;

// ============================================================================
// Test Fixtures
// ============================================================================

/**
 * @brief Create a temporary project directory with .hive-agent/default subdir
 */
fs::path create_test_project() {
    fs::path proj = fs::temp_directory_path() / ("hive_messages_test_" + std::to_string(getpid()));
    fs::create_directories(proj / ".hive-agent" / "default");
    return proj;
}

/**
 * @brief Clean up test project
 */
void cleanup_test_project(const fs::path& proj) {
    fs::remove_all(proj);
}

/**
 * @brief Create Messages with custom config for testing
 */
Messages create_test_messages(const fs::path& proj_root, int keep_head = 2, int keep_tail = 2) {
    MessageCompressionConfig config;
    config.keep_head_count = keep_head;
    config.keep_tail_count = keep_tail;
    config.max_context_size = 200000;
    config.compress_threshold = 0.9f;
    config.debug_output = false;  // Disable debug output in tests
    // §CRASH-2 (2026-07-16): tests disable the background flush thread
    // by default. Most tests want full control over when save() is
    // called (to assert file contents immediately after add_*).
    // Tests that exercise the flush worker re-enable it explicitly
    // by mutating `config` after construction is no longer possible
    // (auto-flush is decided at construction time), so they pass
    // a config with auto_flush_enabled = true via the second
    // overload create_test_messages_cfg() below.
    config.auto_flush_enabled = false;
    // §Phase 8.8 / §Phase 16: Messages takes the per-agent data dir,
    // not the project root. Tests that want the default namespace pass
    // `<proj_root>/.hive-agent/default`. Use
    // create_test_messages_with_name() below to test the named-agent
    // code path.
    return Messages(proj_root / ".hive-agent" / "default", config);
}

/**
 * @brief Create a simple message
 */
json make_msg(const std::string& role, const std::string& content) {
    return json{{"role", role}, {"content", content}};
}

// ============================================================================
// Test Suite: Message Addition
// ============================================================================

TEST_SUITE("Message Addition") {
    
    TEST_CASE("Empty messages on construction") {
        auto proj = create_test_project();
        auto msgs = create_test_messages(proj);
        CHECK(msgs.size() == 0);
        cleanup_test_project(proj);
    }
    
    TEST_CASE("Add system message") {
        auto proj = create_test_project();
        auto msgs = create_test_messages(proj);
        msgs.add_system("You are a helpful assistant.");
        CHECK(msgs.size() == 1);
        CHECK(msgs.get_messages()[0]["role"] == "system");
        cleanup_test_project(proj);
    }
    
    TEST_CASE("Add user message") {
        auto proj = create_test_project();
        auto msgs = create_test_messages(proj);
        msgs.add_user("Hello, how are you?");
        CHECK(msgs.size() == 1);
        CHECK(msgs.get_messages()[0]["role"] == "user");
        cleanup_test_project(proj);
    }
    
    TEST_CASE("Add assistant message") {
        auto proj = create_test_project();
        auto msgs = create_test_messages(proj);
        msgs.add_assistant("I'm doing great!");
        CHECK(msgs.size() == 1);
        CHECK(msgs.get_messages()[0]["role"] == "assistant");
        cleanup_test_project(proj);
    }
    
    TEST_CASE("Add tool result message") {
        auto proj = create_test_project();
        auto msgs = create_test_messages(proj);
        msgs.add_tool_result("call_123", "File content here");
        CHECK(msgs.size() == 1);
        CHECK(msgs.get_messages()[0]["role"] == "tool");
        CHECK(msgs.get_messages()[0]["tool_call_id"] == "call_123");
        cleanup_test_project(proj);
    }
}

// ============================================================================
// Test Suite: Cleanup Dangling Tool Results
// ============================================================================

TEST_SUITE("Cleanup Dangling Tool Results") {
    TEST_CASE("Removes trailing tool result and its assistant") {
        auto proj = create_test_project();
        auto msgs = create_test_messages(proj);

        json chain = json::array();
        chain.push_back(make_msg("system", "You are helpful."));
        chain.push_back(make_msg("user", "Hello"));

        json assistant_msg;
        assistant_msg["role"] = "assistant";
        assistant_msg["content"] = "I'll help.";
        assistant_msg["tool_calls"] = json::array({
            {{"id", "call_1"},
             {"type", "function"},
             {"function", {{"name", "read_file"}, {"arguments", "{}"}}}}
        });
        chain.push_back(assistant_msg);

        json tool_result;
        tool_result["role"] = "tool";
        tool_result["tool_call_id"] = "call_1";
        tool_result["content"] = "File content";
        chain.push_back(tool_result);

        msgs.set_messages(chain);
        CHECK(msgs.size() == 4);

        CHECK(msgs.cleanup_dangling_tool_results() == true);
        CHECK(msgs.size() == 2);
        CHECK(msgs.get_messages()[0]["role"] == "system");
        CHECK(msgs.get_messages()[1]["role"] == "user");

        cleanup_test_project(proj);
    }

    TEST_CASE("Does nothing when last message is not a tool result") {
        auto proj = create_test_project();
        auto msgs = create_test_messages(proj);

        json chain = json::array();
        chain.push_back(make_msg("system", "You are helpful."));
        chain.push_back(make_msg("user", "Hello"));
        chain.push_back(make_msg("assistant", "Hi there!"));

        msgs.set_messages(chain);
        CHECK(msgs.size() == 3);

        CHECK(msgs.cleanup_dangling_tool_results() == false);
        CHECK(msgs.size() == 3);

        cleanup_test_project(proj);
    }
}

// ============================================================================
// Test Suite: Set Messages
// ============================================================================

TEST_SUITE("Set Messages") {
    
    TEST_CASE("Set messages array directly") {
        auto proj = create_test_project();
        auto msgs = create_test_messages(proj);
        
        json test_msgs = json::array();
        test_msgs.push_back(make_msg("system", "You are helpful."));
        test_msgs.push_back(make_msg("user", "Hello"));
        test_msgs.push_back(make_msg("assistant", "Hi there!"));
        
        msgs.set_messages(test_msgs);
        CHECK(msgs.size() == 3);
        CHECK(msgs.get_messages()[0]["content"] == "You are helpful.");
        cleanup_test_project(proj);
    }
}

// ============================================================================
// Test Suite: Split Messages
// ============================================================================

TEST_SUITE("Split Messages - Basic Split") {
    // NOTE: These tests verify the NEW simplified split behavior
    // The new compress() rebuilds as: system + user(instruction) + summary
    // split_messages() is now DEPRECATED - kept for debugging only
    // New behavior: prefix = system (index 0), middle = all others, suffix = empty
    
    TEST_CASE("Split with 6 messages - normal case") {
        // With the NEW 50/50 split:
        // prefix = system (1 msg), middle = first 50% of non-system (3), suffix = last 50% (3)
        auto proj = create_test_project();
        auto msgs = create_test_messages(proj, 2, 2);
        
        json test_msgs = json::array();
        test_msgs.push_back(make_msg("system", "System"));
        test_msgs.push_back(make_msg("user", "Task 1"));
        test_msgs.push_back(make_msg("assistant", "Response 1"));
        test_msgs.push_back(make_msg("user", "Task 2"));  // middle (first half)
        test_msgs.push_back(make_msg("assistant", "Response 2"));  // middle (first half)
        test_msgs.push_back(make_msg("user", "Task 3"));  // suffix (second half)
        test_msgs.push_back(make_msg("assistant", "Response 3"));  // suffix (second half)
        
        msgs.set_messages(test_msgs);
        CHECK(msgs.size() == 7);
        
        size_t prefix_size, middle_size, suffix_size;
        msgs.get_split_info(prefix_size, middle_size, suffix_size);
        
        // NEW 70/30 split: prefix=1 (system), middle=4 (first 70%), suffix=2 (last 30%)
        CHECK(prefix_size == 1);    // system only
        CHECK(middle_size == 4);    // first 70% of non-system
        CHECK(suffix_size == 2);   // last 30% of non-system
        cleanup_test_project(proj);
    }

    TEST_CASE("Split with 5 messages - overlap case") {
        // NEW 50/50 split: 4 non-system messages split as 2/2
        auto proj = create_test_project();
        auto msgs = create_test_messages(proj, 2, 2);
        
        json test_msgs = json::array();
        test_msgs.push_back(make_msg("system", "System"));
        test_msgs.push_back(make_msg("user", "Task 1"));    // middle (first half)
        test_msgs.push_back(make_msg("assistant", "Response 1"));  // middle (first half)
        test_msgs.push_back(make_msg("user", "Task 2"));    // suffix (second half)
        test_msgs.push_back(make_msg("assistant", "Response 2"));  // suffix (second half)
        
        msgs.set_messages(test_msgs);
        CHECK(msgs.size() == 5);
        
        size_t prefix_size, middle_size, suffix_size;
        msgs.get_split_info(prefix_size, middle_size, suffix_size);
        
        // NEW 50/50 split: prefix=1, middle=2, suffix=2
        CHECK(prefix_size == 1);     // system only
        CHECK(middle_size == 2);     // first half of non-system
        CHECK(suffix_size == 2);     // second half of non-system
        cleanup_test_project(proj);
    }
    
    TEST_CASE("Split with 4 messages - edge case") {
        // NEW 50/50 split: 3 non-system messages split as 1/2
        auto proj = create_test_project();
        auto msgs = create_test_messages(proj, 2, 2);
        
        json test_msgs = json::array();
        test_msgs.push_back(make_msg("system", "System"));
        test_msgs.push_back(make_msg("user", "Task 1"));    // middle (first half)
        test_msgs.push_back(make_msg("assistant", "Response 1"));  // suffix (second half)
        test_msgs.push_back(make_msg("user", "Task 2"));    // suffix (second half)
        
        msgs.set_messages(test_msgs);
        CHECK(msgs.size() == 4);
        
        size_t prefix_size, middle_size, suffix_size;
        msgs.get_split_info(prefix_size, middle_size, suffix_size);
        
        // NEW 70/30 split: prefix=1, middle=2, suffix=1
        CHECK(prefix_size == 1);     // system only
        CHECK(middle_size == 2);     // first 70% of non-system
        CHECK(suffix_size == 1);     // last 30% of non-system
        cleanup_test_project(proj);
    }
    
    TEST_CASE("Split with 3 messages - edge case (all in prefix)") {
        // NEW 50/50 split: 2 non-system messages split as 1/1
        auto proj = create_test_project();
        auto msgs = create_test_messages(proj, 2, 2);
        
        json test_msgs = json::array();
        test_msgs.push_back(make_msg("system", "System"));
        test_msgs.push_back(make_msg("user", "Task 1"));    // middle (first half)
        test_msgs.push_back(make_msg("assistant", "Response 1"));  // suffix (second half)
        
        msgs.set_messages(test_msgs);
        CHECK(msgs.size() == 3);
        
        size_t prefix_size, middle_size, suffix_size;
        msgs.get_split_info(prefix_size, middle_size, suffix_size);
        
        // NEW 50/50 split: prefix=1 (system), middle=1, suffix=1
        CHECK(prefix_size == 1);
        CHECK(middle_size == 1);
        CHECK(suffix_size == 1);
        cleanup_test_project(proj);
    }
}

// ============================================================================
// Test Suite: Split Messages - Custom Config
// ============================================================================

TEST_SUITE("Split Messages - Custom Config") {
    // NOTE: The keep_head_count and keep_tail_count are DEPRECATED
    // The new compress() ignores these values
    // New behavior: prefix = system only, middle = all others, suffix = empty
    
    TEST_CASE("Split with keep_head=1, keep_tail=1") {
        // NEW 50/50 split: 7 non-system → 3 middle, 4 suffix
        auto proj = create_test_project();
        auto msgs = create_test_messages(proj, 1, 1);
        
        json test_msgs = json::array();
        test_msgs.push_back(make_msg("system", "System"));  // prefix
        test_msgs.push_back(make_msg("user", "Msg 0"));    // middle (first half)
        test_msgs.push_back(make_msg("user", "Msg 1"));    // middle (first half)
        test_msgs.push_back(make_msg("user", "Msg 2"));    // middle (first half)
        test_msgs.push_back(make_msg("user", "Msg 3"));    // suffix (second half)
        test_msgs.push_back(make_msg("user", "Msg 4"));    // suffix (second half)
        test_msgs.push_back(make_msg("user", "Msg 5"));    // suffix (second half)
        test_msgs.push_back(make_msg("user", "Msg 6"));    // suffix (second half)
        
        msgs.set_messages(test_msgs);
        CHECK(msgs.size() == 8);
        
        size_t prefix_size, middle_size, suffix_size;
        msgs.get_split_info(prefix_size, middle_size, suffix_size);
        
        // NEW 70/30 split: prefix=1 (system), middle=4 (first 70%), suffix=3 (last 30%)
        CHECK(prefix_size == 1);    // system only
        CHECK(middle_size == 4);    // first 70% of non-system
        CHECK(suffix_size == 3);   // last 30% of non-system
        cleanup_test_project(proj);
    }

    TEST_CASE("Split with keep_head=3, keep_tail=1") {
        // NEW 50/50 split: 9 non-system → 4 middle, 5 suffix
        auto proj = create_test_project();
        auto msgs = create_test_messages(proj, 3, 1);
        
        json test_msgs = json::array();
        test_msgs.push_back(make_msg("system", "System"));  // prefix
        test_msgs.push_back(make_msg("user", "Msg 0"));    // middle (first half)
        test_msgs.push_back(make_msg("user", "Msg 1"));    // middle (first half)
        test_msgs.push_back(make_msg("user", "Msg 2"));    // middle (first half)
        test_msgs.push_back(make_msg("user", "Msg 3"));    // middle (first half)
        test_msgs.push_back(make_msg("user", "Msg 4"));    // suffix (second half)
        test_msgs.push_back(make_msg("user", "Msg 5"));    // suffix (second half)
        test_msgs.push_back(make_msg("user", "Msg 6"));    // suffix (second half)
        test_msgs.push_back(make_msg("user", "Msg 7"));    // suffix (second half)
        test_msgs.push_back(make_msg("user", "Msg 8"));    // suffix (second half)
        
        msgs.set_messages(test_msgs);
        
        size_t prefix_size, middle_size, suffix_size;
        msgs.get_split_info(prefix_size, middle_size, suffix_size);
        
        // NEW 70/30 split: prefix=1 (system), middle=6, suffix=3
        CHECK(prefix_size == 1);     // system only
        CHECK(middle_size == 6);     // first 70% of non-system
        CHECK(suffix_size == 3);     // last 30% of non-system
        cleanup_test_project(proj);
    }
    
    TEST_CASE("Split with keep_head=1, keep_tail=3") {
        // NEW 50/50 split: 9 non-system → 4 middle, 5 suffix (ignores config)
        auto proj = create_test_project();
        auto msgs = create_test_messages(proj, 1, 3);
        
        json test_msgs = json::array();
        test_msgs.push_back(make_msg("system", "System"));  // prefix
        test_msgs.push_back(make_msg("user", "Msg 0"));    // middle (first half)
        test_msgs.push_back(make_msg("user", "Msg 1"));    // middle (first half)
        test_msgs.push_back(make_msg("user", "Msg 2"));    // middle (first half)
        test_msgs.push_back(make_msg("user", "Msg 3"));    // middle (first half)
        test_msgs.push_back(make_msg("user", "Msg 4"));    // suffix (second half)
        test_msgs.push_back(make_msg("user", "Msg 5"));    // suffix (second half)
        test_msgs.push_back(make_msg("user", "Msg 6"));    // suffix (second half)
        test_msgs.push_back(make_msg("user", "Msg 7"));    // suffix (second half)
        test_msgs.push_back(make_msg("user", "Msg 8"));    // suffix (second half)
        
        msgs.set_messages(test_msgs);
        
        size_t prefix_size, middle_size, suffix_size;
        msgs.get_split_info(prefix_size, middle_size, suffix_size);
        
        // NEW 70/30 split: prefix=1 (system), middle=6 (first 70%), suffix=3 (last 30%)
        CHECK(prefix_size == 1);     // system only
        CHECK(middle_size == 6);     // first 70% of non-system
        CHECK(suffix_size == 3);     // last 30% of non-system
        cleanup_test_project(proj);
    }
}

// ============================================================================
// Test Suite: Replace Messages
// ============================================================================

TEST_SUITE("Replace Messages") {
    
    TEST_CASE("Replace with valid JSON string") {
        auto proj = create_test_project();
        auto msgs = create_test_messages(proj);
        
        msgs.add_system("Original system");
        CHECK(msgs.size() == 1);
        
        // Create compressed JSON (simulating compression result)
        json compressed = json::array();
        compressed.push_back(make_msg("system", "Original system"));  // Keep system
        compressed.push_back(make_msg("user", "Original task"));  // prefix
        compressed.push_back(make_msg("assistant", "Summary of middle messages..."));  // summary replaces middle
        
        std::string compressed_str = compressed.dump();
        msgs.replace_messages(compressed_str);
        
        CHECK(msgs.size() == 3);
        CHECK(msgs.get_messages()[2]["content"] == "Summary of middle messages...");
        cleanup_test_project(proj);
    }
    
    TEST_CASE("Replace with invalid JSON - graceful handling") {
        auto proj = create_test_project();
        auto msgs = create_test_messages(proj);
        
        msgs.add_system("Original");
        msgs.add_user("Test");
        CHECK(msgs.size() == 2);
        
        // Send invalid JSON - should not throw, just log error
        msgs.replace_messages("not valid json {{{");
        
        // Original messages should be unchanged
        CHECK(msgs.size() == 2);
        cleanup_test_project(proj);
    }
    
    TEST_CASE("Replace with empty array") {
        auto proj = create_test_project();
        auto msgs = create_test_messages(proj);
        
        msgs.add_system("Should be kept");
        CHECK(msgs.size() == 1);
        
        // Replace with empty array
        msgs.replace_messages("[]");
        
        CHECK(msgs.size() == 0);
        cleanup_test_project(proj);
    }
}

// ============================================================================
// Test Suite: Replace Messages - Compression Workflow
// ============================================================================

TEST_SUITE("Replace Messages - Compression Workflow") {
    
    TEST_CASE("Simulate compression workflow: set -> split info -> replace") {
        auto proj = create_test_project();
        auto msgs = create_test_messages(proj, 2, 2);
        
        // Build up messages like a real session
        msgs.add_system("You are CodeMax.");
        msgs.add_user("Build a calculator");
        msgs.add_assistant("I'll create a calculator app.");
        msgs.add_user("Add addition function");
        msgs.add_assistant("Added add() function.");
        msgs.add_user("Add multiplication function");
        msgs.add_assistant("Added multiply() function.");
        msgs.add_user("Build the project");
        msgs.add_assistant("Project builds successfully.");
        
        CHECK(msgs.size() == 9);
        
        // Verify split before compression
        size_t prefix_size, middle_size, suffix_size;
        msgs.get_split_info(prefix_size, middle_size, suffix_size);
        
        // Should have middle to compress
        CHECK(middle_size > 0);
        
        // Simulate compression: keep prefix + summary + suffix
        json compressed = json::array();
        // Keep first 3 messages (system + prefix)
        for (int i = 0; i < 3; i++) {
            compressed.push_back(msgs.get_messages()[i]);
        }
        // Add summary
        compressed.push_back(make_msg("assistant", "[COMPRESSED] Summary: Created calculator with add and multiply functions. Build verified."));
        // Keep suffix (last 2 messages)
        compressed.push_back(msgs.get_messages()[7]);
        compressed.push_back(msgs.get_messages()[8]);
        
        msgs.replace_messages(compressed.dump());
        
        // After compression: system + prefix(2) + summary(1) + suffix(2) = 6 messages
        CHECK(msgs.size() == 6);
        
        // Verify the summary message is there
        bool found_summary = false;
        for (const auto& msg : msgs.get_messages()) {
            if (msg["content"].get<std::string>().find("[COMPRESSED]") != std::string::npos) {
                found_summary = true;
                break;
            }
        }
        CHECK(found_summary);
        
        cleanup_test_project(proj);
    }
}

// ============================================================================
// Test Suite: Edge Cases
// ============================================================================

TEST_SUITE("Edge Cases") {
    
    TEST_CASE("Empty messages array") {
        auto proj = create_test_project();
        auto msgs = create_test_messages(proj, 2, 2);
        
        size_t prefix_size, middle_size, suffix_size;
        msgs.get_split_info(prefix_size, middle_size, suffix_size);
        
        CHECK(prefix_size == 0);
        CHECK(middle_size == 0);
        CHECK(suffix_size == 0);
        cleanup_test_project(proj);
    }
    
    TEST_CASE("Only system message (1 message)") {
        auto proj = create_test_project();
        auto msgs = create_test_messages(proj, 2, 2);
        msgs.add_system("Only system");
        
        size_t prefix_size, middle_size, suffix_size;
        msgs.get_split_info(prefix_size, middle_size, suffix_size);
        
        // Edge case: total(1) <= head_end(3)? Yes -> all in prefix
        CHECK(prefix_size == 1);
        CHECK(middle_size == 0);
        CHECK(suffix_size == 0);
        cleanup_test_project(proj);
    }
    
    TEST_CASE("System + just enough for head (3 messages)") {
        auto proj = create_test_project();
        auto msgs = create_test_messages(proj, 2, 2);
        msgs.add_system("System");
        msgs.add_user("Task");
        msgs.add_assistant("Done");
        
        size_t prefix_size, middle_size, suffix_size;
        msgs.get_split_info(prefix_size, middle_size, suffix_size);
        
        // NEW 50/50 split: 2 non-system → 1 middle, 1 suffix
        CHECK(prefix_size == 1);
        CHECK(middle_size == 1);
        CHECK(suffix_size == 1);
        cleanup_test_project(proj);
    }
    
    TEST_CASE("Large message count") {
        auto proj = create_test_project();
        auto msgs = create_test_messages(proj, 2, 2);
        
        // Add system + 100 messages
        msgs.add_system("System");
        for (int i = 0; i < 100; i++) {
            msgs.add_user("Message " + std::to_string(i));
        }
        
        CHECK(msgs.size() == 101);
        
        size_t prefix_size, middle_size, suffix_size;
        msgs.get_split_info(prefix_size, middle_size, suffix_size);
        
        // NEW 70/30 split: prefix=1 (system), middle=70, suffix=30
        CHECK(prefix_size == 1);
        CHECK(middle_size == 70);
        CHECK(suffix_size == 30);
        cleanup_test_project(proj);
    }
}

// ============================================================================
// Test Suite: Compression Threshold
// ============================================================================

TEST_SUITE("Compression Threshold") {
    
    TEST_CASE("should_compress at 90% threshold") {
        auto proj = create_test_project();
        auto msgs = create_test_messages(proj, 2, 2);
        
        msgs.add_system("System");
        msgs.add_user("Task");
        msgs.add_assistant("Response");
        
        // Default config: compress_threshold=0.9, max_context_size=200000
        CHECK_FALSE(msgs.should_compress(150000));  // 75% - below threshold
        CHECK_FALSE(msgs.should_compress(179999)); // 89.99% - below threshold
        CHECK(msgs.should_compress(180000));       // 90% - at threshold
        CHECK(msgs.should_compress(200000));        // 100% - at max
        cleanup_test_project(proj);
    }
    
    TEST_CASE("should_compress with custom threshold") {
        auto proj = create_test_project();
        MessageCompressionConfig config;
        config.compress_threshold = 0.5f;  // 50%
        config.max_context_size = 100000;
        // §CRASH-2 (2026-07-16): disable auto-flush so the background
        // worker doesn't race with cleanup_test_project below.
        config.auto_flush_enabled = false;
        Messages msgs(proj, config);

        msgs.add_system("System");
        msgs.add_user("Task");

        CHECK_FALSE(msgs.should_compress(49000));  // 49%
        CHECK(msgs.should_compress(50000));        // 50%
        CHECK(msgs.should_compress(100000));       // 100%
        cleanup_test_project(proj);
    }
}

// ============================================================================
// Test Suite: Token Estimation
// ============================================================================

TEST_SUITE("Token Estimation") {
    
    TEST_CASE("estimate_tokens returns reasonable value") {
        auto proj = create_test_project();
        auto msgs = create_test_messages(proj);
        
        msgs.add_system("Short system message");
        msgs.add_user("Short user message");
        
        size_t tokens = msgs.estimate_tokens();
        
        // Should be non-zero and roughly proportional to content size
        CHECK(tokens > 0);
        CHECK(tokens < 1000);  // Very rough upper bound for small messages
        cleanup_test_project(proj);
    }
    
    TEST_CASE("estimate_tokens with large content") {
        auto proj = create_test_project();
        auto msgs = create_test_messages(proj);
        
        // Add a larger message (~1000 chars = ~250 tokens estimate)
        std::string large_content(1000, 'x');
        msgs.add_user(large_content);
        
        size_t tokens = msgs.estimate_tokens();
        
        // 1000 chars / 4 = ~250 tokens estimate
        CHECK(tokens > 200);
        CHECK(tokens < 350);
        cleanup_test_project(proj);
    }
}

// ============================================================================
// Test Suite: Save and Load (Persistence)
// ============================================================================

TEST_SUITE("Save and Load") {
    
    TEST_CASE("has_saved_messages returns false when no file exists") {
        auto proj = create_test_project();
        auto msgs = create_test_messages(proj);
        
        CHECK_FALSE(msgs.has_saved_messages());
        cleanup_test_project(proj);
    }
    
    TEST_CASE("save creates messages file") {
        auto proj = create_test_project();
        auto msgs = create_test_messages(proj);
        
        msgs.add_system("System message");
        msgs.add_user("User message");
        msgs.add_assistant("Assistant response");
        
        CHECK_FALSE(msgs.has_saved_messages());  // Not saved yet
        msgs.save();
        CHECK(msgs.has_saved_messages());  // Now saved
        
        cleanup_test_project(proj);
    }
    
    TEST_CASE("save and load round-trip") {
        auto proj = create_test_project();
        auto msgs1 = create_test_messages(proj);
        
        // Add various message types
        msgs1.add_system("System prompt here");
        msgs1.add_user("User input with special chars: äöü");
        msgs1.add_assistant("Assistant response with more text");
        // Add proper tool call chain: assistant with tool_calls + tool result
        msgs1.get_messages().push_back({
            {"role", "assistant"},
            {"tool_calls", json::array({
                {
                    {"id", "tool_call_123"},
                    {"type", "function"},
                    {"function", {
                        {"name", "test_tool"},
                        {"arguments", "{}"}
                    }}
                }
            })}
        });
        msgs1.add_tool_result("tool_call_123", "Tool result content");
        
        msgs1.save();
        
        // Create new Messages instance and load
        auto msgs2 = create_test_messages(proj);
        CHECK(msgs2.load());  // Should successfully load
        
        // Verify message count and content
        // Structure: system + user + assistant + assistant(tool_calls) + tool_result = 5
        CHECK(msgs2.size() == 5);
        CHECK(msgs2.get_messages()[0]["content"] == "System prompt here");
        CHECK(msgs2.get_messages()[1]["content"] == "User input with special chars: äöü");
        CHECK(msgs2.get_messages()[2]["content"] == "Assistant response with more text");
        CHECK(msgs2.get_messages()[3]["role"] == "assistant");
        CHECK(msgs2.get_messages()[3].contains("tool_calls"));
        // Tool result is at index 4 (the assistant with tool_calls at 3 has matching result)
        CHECK(msgs2.get_messages()[4]["role"] == "tool");
        CHECK(msgs2.get_messages()[4]["tool_call_id"] == "tool_call_123");
        
        cleanup_test_project(proj);
    }
    
    TEST_CASE("load returns false when no file exists") {
        auto proj = create_test_project();
        auto msgs = create_test_messages(proj);
        
        CHECK_FALSE(msgs.load());
        cleanup_test_project(proj);
    }
    
    TEST_CASE("load returns false for empty file") {
        auto proj = create_test_project();
        
        // Create empty messages file
        fs::path agent_dir = proj / ".hive-agent" / "default";
        fs::create_directories(agent_dir);
        std::ofstream f(agent_dir / "messages.json");
        f << "";  // Empty file
        f.close();
        
        auto msgs = create_test_messages(proj);
        CHECK_FALSE(msgs.load());
        
        cleanup_test_project(proj);
    }
    
    TEST_CASE("load returns false for invalid JSON") {
        auto proj = create_test_project();
        
        // Create invalid JSON file
        fs::path agent_dir = proj / ".hive-agent" / "default";
        fs::create_directories(agent_dir);
        std::ofstream f(agent_dir / "messages.json");
        f << "this is not json";  // Invalid JSON
        f.close();
        
        auto msgs = create_test_messages(proj);
        CHECK_FALSE(msgs.load());
        
        cleanup_test_project(proj);
    }
    
    TEST_CASE("load returns false for non-array JSON") {
        auto proj = create_test_project();
        
        // Create JSON object instead of array
        fs::path agent_dir = proj / ".hive-agent" / "default";
        fs::create_directories(agent_dir);
        std::ofstream f(agent_dir / "messages.json");
        f << "{\"invalid\": true}";  // Not an array
        f.close();
        
        auto msgs = create_test_messages(proj);
        CHECK_FALSE(msgs.load());
        
        cleanup_test_project(proj);
    }
    
    TEST_CASE("save does not create file for empty messages") {
        auto proj = create_test_project();
        auto msgs = create_test_messages(proj);
        
        CHECK_FALSE(msgs.has_saved_messages());
        msgs.save();  // Should not create file for empty messages
        CHECK_FALSE(msgs.has_saved_messages());
        
        cleanup_test_project(proj);
    }
    
    TEST_CASE("clear persists the cleared state (file removed immediately)") {
        auto proj = create_test_project();
        auto msgs = create_test_messages(proj);
        
        msgs.add_system("Test");
        msgs.save();
        CHECK(msgs.has_saved_messages());
        
        msgs.clear();
        CHECK(msgs.size() == 0);
        // §CRASH-6 (defect #6): clear() persists synchronously — the
        // file (and the SQLite mirror row) are gone without an
        // explicit save().
        CHECK_FALSE(msgs.has_saved_messages());
        // The crash payload is cleared too, so the signal handler
        // cannot resurrect the cleared chain.
        CHECK(msgs.crash_payload_for_signal().empty());
        
        cleanup_test_project(proj);
    }

    TEST_CASE("CRASH-6 clear survives reload (cleared chain not resurrected)") {
        auto proj = create_test_project();

        {
            auto msgs = create_test_messages(proj);
            msgs.add_system("Doomed system");
            msgs.add_user("Doomed user");
            msgs.save();
            CHECK(msgs.has_saved_messages());
            msgs.clear();
        }

        // A fresh instance must NOT see the cleared chain (regression:
        // clear() used to leave messages.json + the SQLite mirror
        // untouched, so load() brought the old chain back).
        auto msgs2 = create_test_messages(proj);
        CHECK_FALSE(msgs2.load());
        CHECK(msgs2.size() == 0);

        cleanup_test_project(proj);
    }

    TEST_CASE("CRASH-6 load drops structurally invalid messages") {
        auto proj = create_test_project();

        // Plant a messages.json where one message is missing its role
        // (the monkeybyte failure mode: every LLM request built from
        // such a chain was rejected by the provider, forever).
        {
            std::ofstream f(proj / ".hive-agent" / "default" / "messages.json",
                            std::ios::binary);
            f << R"([
                {"role":"system","content":"System"},
                {"content":"assistant reply with no role"},
                {"role":"user","content":"Hello"}
            ])";
        }

        auto msgs = create_test_messages(proj);
        CHECK(msgs.load());
        // The invalid message is dropped, the valid ones survive.
        CHECK(msgs.size() == 2);
        CHECK(msgs.get_messages()[0]["role"] == "system");
        CHECK(msgs.get_messages()[1]["role"] == "user");

        cleanup_test_project(proj);
    }

    TEST_CASE("CRASH-6 load with only invalid messages starts fresh") {
        auto proj = create_test_project();

        {
            std::ofstream f(proj / ".hive-agent" / "default" / "messages.json",
                            std::ios::binary);
            f << R"([{"content":"no role at all"}])";
        }

        auto msgs = create_test_messages(proj);
        CHECK_FALSE(msgs.load());
        CHECK(msgs.size() == 0);

        cleanup_test_project(proj);
    }

    TEST_CASE("CRASH-6 .crash mirror tolerates a stale trailing tail") {
        auto proj = create_test_project();

        // Plant a .crash file with a stale tail (what a failed
        // ftruncate leaves behind — a suffix of a previous, longer
        // payload, defect #5).
        {
            std::ofstream f(proj / ".hive-agent" / "default" / "messages.json.crash",
                            std::ios::binary);
            f << R"([{"role":"system","content":"Recovered"}]n":"old stale tail"}])";
        }

        MessageCompressionConfig cfg;
        cfg.auto_flush_enabled = false;
        Messages msgs(proj / ".hive-agent" / "default", cfg);
        CHECK(msgs.load());
        CHECK(msgs.size() == 1);
        CHECK(msgs.get_messages()[0]["content"] == "Recovered");

        cleanup_test_project(proj);
    }
    
    TEST_CASE("messages persist after destruction and recreation") {
        auto proj = create_test_project();

        // Create, save messages, then destruct
        {
            auto msgs1 = create_test_messages(proj);
            msgs1.add_system("Persistent system");
            msgs1.add_user("Persistent user");
            msgs1.save();
        }

        // Create new instance and load
        auto msgs2 = create_test_messages(proj);
        CHECK(msgs2.load());
        CHECK(msgs2.size() == 2);
        CHECK(msgs2.get_messages()[0]["content"] == "Persistent system");

        cleanup_test_project(proj);
    }

    // §CRASH-1 (2026-07-16): atomic save via temp+rename.

    TEST_CASE("CRASH-1 save leaves no .tmp file behind on success") {
        auto proj = create_test_project();
        auto msgs = create_test_messages(proj);

        msgs.add_system("Atomic save test");
        msgs.add_user("Hello");
        msgs.save();

        // Final file exists.
        CHECK(msgs.has_saved_messages());
        // No .tmp left behind.
        CHECK_FALSE(std::filesystem::exists(
                        proj / ".hive-agent" / "default" / "messages.json.tmp"));
        // .crash mirror was also written.
        CHECK(std::filesystem::exists(
                  proj / ".hive-agent" / "default" / "messages.json.crash"));

        cleanup_test_project(proj);
    }

    TEST_CASE("CRASH-1 save recovers from a stale .tmp file") {
        auto proj = create_test_project();
        auto msgs_dir = proj / ".hive-agent" / "default";
        auto msgs = create_test_messages(proj);

        // Pre-create a stale .tmp file from a "previous incomplete save".
        // It contains garbage; the new save() must overwrite it.
        std::ofstream bad(msgs_dir / "messages.json.tmp", std::ios::binary);
        bad << "{ this is not valid json";
        bad.close();

        msgs.add_system("Re-overwrites stale .tmp");
        msgs.add_user("Hello");
        // If save() doesn't overwrite .tmp, this would also fail because
        // the rename() target is busy. The test passes only if save()
        // trumps/overwrites stale .tmp.
        CHECK_NOTHROW(msgs.save());
        CHECK(msgs.has_saved_messages());

        // Reload — must parse cleanly.
        auto msgs2 = create_test_messages(proj);
        CHECK(msgs2.load());
        CHECK(msgs2.size() == 2);

        cleanup_test_project(proj);
    }

    TEST_CASE("CRASH-1 save produces valid file even after a previous truncation") {
        auto proj = create_test_project();
        auto msgs_file = proj / ".hive-agent" / "default" / "messages.json";

        // First save: write valid content.
        {
            auto msgs = create_test_messages(proj);
            msgs.add_system("First");
            msgs.add_user("Hello");
            msgs.save();
            CHECK(msgs.has_saved_messages());
        }
        // Simulate a previously-crashed save that left a truncated
        // file on disk.
        {
            std::ofstream f(msgs_file, std::ios::binary | std::ios::trunc);
            f << "[{\"role\":\"user\",\"content\":\"HALF-WRITTEN";  // truncated JSON
            // Do NOT close — simulate the file as it would look on
            // disk after a kill mid-write. close() will flush; the
            // partial content is what load() will see.
            f.close();
        }

        // Save again. Should atomically replace the truncated file.
        {
            auto msgs = create_test_messages(proj);
            msgs.add_system("Second");
            msgs.add_user("World");
            msgs.save();
        }
        // Load — must see the second save, not the truncated first.
        {
            auto msgs = create_test_messages(proj);
            CHECK(msgs.load());
            CHECK(msgs.size() == 2);
            CHECK(msgs.get_messages()[0]["content"] == "Second");
            CHECK(msgs.get_messages()[1]["content"] == "World");
        }

        cleanup_test_project(proj);
    }

    TEST_CASE("CRASH-1 empty-session save also cleans up .crash file") {
        auto proj = create_test_project();
        auto crash = proj / ".hive-agent" / "default" / "messages.json.crash";

        // Pre-plant a stale .crash from a previous session.
        std::ofstream cf(crash, std::ios::binary);
        cf << "[\"stale\"]";
        cf.close();
        CHECK(std::filesystem::exists(crash));

        // New session with no messages; save() must remove the
        // stale .crash so the next load() doesn't pick it up.
        auto msgs = create_test_messages(proj);
        msgs.save();
        CHECK_FALSE(std::filesystem::exists(crash));

        cleanup_test_project(proj);
    }

    // §CRASH-2 (2026-07-16): write-through auto-flush tests.

    TEST_CASE("CRASH-2 background flush worker writes within 100ms") {
        auto proj = create_test_project();

        // Build a Messages with auto_flush_enabled = true and a
        // tight 50ms interval so the test runs fast.
        MessageCompressionConfig cfg;
        cfg.auto_flush_enabled = true;
        cfg.auto_flush_interval_ms = 50;
        Messages msgs(proj / ".hive-agent" / "default", cfg);

        msgs.add_system("Background flush test");
        msgs.add_user("Hello");

        // Poll for up to 1 second for the file to appear.
        const auto deadline = std::chrono::steady_clock::now()
                              + std::chrono::seconds(1);
        bool appeared = false;
        while (std::chrono::steady_clock::now() < deadline) {
            if (msgs.has_saved_messages()) {
                appeared = true;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        CHECK(appeared);

        // Verify contents survived the round-trip via the background
        // thread (not via explicit save()).
        Messages reload(proj / ".hive-agent" / "default", cfg);
        CHECK(reload.load());
        CHECK(reload.size() == 2);
        CHECK(reload.get_messages()[1]["content"] == "Hello");

        cleanup_test_project(proj);
    }

    TEST_CASE("CRASH-2 flush_now() blocks until save completes") {
        auto proj = create_test_project();
        MessageCompressionConfig cfg;
        cfg.auto_flush_enabled = true;
        cfg.auto_flush_interval_ms = 10000;  // effectively never auto-flush
        Messages msgs(proj / ".hive-agent" / "default", cfg);

        msgs.add_user("Synchronous flush please");
        // The background worker won't fire for 10s; only an
        // explicit flush_now() should put it on disk.
        CHECK_FALSE(msgs.has_saved_messages());
        msgs.flush_now();
        CHECK(msgs.has_saved_messages());

        // Reload to confirm contents.
        MessageCompressionConfig cfg2 = cfg;
        Messages reload(proj / ".hive-agent" / "default", cfg2);
        CHECK(reload.load());
        CHECK(reload.get_messages()[0]["content"] == "Synchronous flush please");

        cleanup_test_project(proj);
    }

    TEST_CASE("CRASH-2 destructor stops worker cleanly") {
        auto proj = create_test_project();
        MessageCompressionConfig cfg;
        cfg.auto_flush_enabled = true;
        cfg.auto_flush_interval_ms = 50;
        // Add a message, then destroy immediately.
        {
            Messages msgs(proj / ".hive-agent" / "default", cfg);
            msgs.add_system("Destroy me");
            msgs.add_user("Last gasp");
            // No explicit flush_now() — relies on destructor's
            // worker-stop-and-final-flush path.
        }
        // File MUST exist on disk after Messages goes out of scope.
        CHECK(std::filesystem::exists(proj / ".hive-agent" / "default" / "messages.json"));

        cleanup_test_project(proj);
    }

    TEST_CASE("CRASH-2 auto_flush disabled leaves save to caller") {
        auto proj = create_test_project();
        // create_test_messages() already disables auto-flush.
        auto msgs = create_test_messages(proj);
        msgs.add_user("Should not auto-flush");
        // Background worker not running, so no save happens.
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        CHECK_FALSE(msgs.has_saved_messages());
        // Explicit save() still works.
        msgs.save();
        CHECK(msgs.has_saved_messages());

        cleanup_test_project(proj);
    }

    // §CRASH-4 (2026-07-16): crash handler install/uninstall.

    TEST_CASE("CRASH-4 install_crash_handlers opens the .crash file") {
        auto proj = create_test_project();
        MessageCompressionConfig cfg;
        cfg.auto_flush_enabled = false;
        Messages msgs(proj / ".hive-agent" / "default", cfg);

        msgs.add_system("Crash test");
        msgs.save();   // populates crash_payload_

        msgs.install_crash_handlers();

        const std::string crash_path =
            (proj / ".hive-agent" / "default" / "messages.json.crash").string();
        CHECK(std::filesystem::exists(crash_path));
        // The .crash file should be non-empty (we saved above).
        CHECK(std::filesystem::file_size(crash_path) > 0);

        msgs.uninstall_crash_handlers();
        cleanup_test_project(proj);
    }

    TEST_CASE("CRASH-4 install is idempotent (no double-register)") {
        auto proj = create_test_project();
        MessageCompressionConfig cfg;
        cfg.auto_flush_enabled = false;
        Messages msgs(proj / ".hive-agent" / "default", cfg);

        msgs.add_system("Idempotent test");
        msgs.save();

        // Install twice — the signal handler is registered exactly once.
        msgs.install_crash_handlers();
        msgs.install_crash_handlers();
        // No crash; no need to assert here.

        msgs.uninstall_crash_handlers();
        cleanup_test_project(proj);
    }

    TEST_CASE("CRASH-4 uninstall is safe to call without install") {
        auto proj = create_test_project();
        MessageCompressionConfig cfg;
        cfg.auto_flush_enabled = false;
        Messages msgs(proj / ".hive-agent" / "default", cfg);

        // Never installed. Uninstall must be a no-op, not crash.
        CHECK_NOTHROW(msgs.uninstall_crash_handlers());

        cleanup_test_project(proj);
    }

    TEST_CASE("CRASH-4 crash_payload_for_signal reflects last save") {
        auto proj = create_test_project();
        MessageCompressionConfig cfg;
        cfg.auto_flush_enabled = false;
        Messages msgs(proj / ".hive-agent" / "default", cfg);

        msgs.add_system("First save");
        msgs.add_user("Hello");
        msgs.save();
        // Cached payload should contain the messages we just saved.
        const std::string& blob = msgs.crash_payload_for_signal();
        CHECK(blob.find("\"First save\"") != std::string::npos);
        CHECK(blob.find("\"Hello\"") != std::string::npos);

        cleanup_test_project(proj);
    }
}

// §CRASH-4 fork-based integration test (skipped by default — see note).
//
// We previously tried to fork a child that installs the crash handler,
// saves messages, then calls std::abort() to trigger SIGABRT. The
// signal handler runs, writes .crash, re-raises, and the child dies.
// The parent verifies the .crash file contents.
//
// The test DOES verify the crash handler end-to-end, but it's
// tricky inside doctest because:
//   1. libc's std::abort() flushes stdio streams (NOT async-signal-
//      safe — could deadlock if the child inherits our write paths).
//   2. doctest installs its own SIGABRT handler that calls
//      std::exit(EXIT_FAILURE) — when our handler re-raises SIGABRT
//      with SIG_DFL, the kernel delivers it; if anything in that
//      delivery still triggers the previous (doctest) handler, the
//      parent process aborts.
//   3. After install_crash_handlers, the parent's SIGABRT handler
//      is permanently changed to ours — earlier CRASH-4 tests leave
//      it set, and that state persists.
//
// The component-level unit tests (CRASH-4 install/uninstall/...
// above) verify the machinery. End-to-end verification is done
// in a SEPARATE standalone binary under
// `demos/daedalus-cli/crash_recovery_e2e/` (built ad hoc). For
// now, skip the in-process fork variant.
TEST_CASE("CRASH-4 fork: SIGABRT writes messages.json.crash"
          * doctest::skip(true)) {
    // Placeholder — see note above.
    MESSAGE("See demos/daedalus-cli/crash_recovery_e2e/ for the");
    MESSAGE("end-to-end fork-based crash handler test.");
    CHECK(true);
}

// §CRASH-5 (2026-07-16): drop-last-message recovery heuristic tests.

TEST_CASE("CRASH-5 load falls back to .crash when messages.json missing") {
    auto proj = create_test_project();
    auto crash_path = proj / ".hive-agent" / "default" / "messages.json.crash";

    // Plant a valid .crash file; no messages.json.
    {
        std::ofstream f(crash_path, std::ios::binary);
        f << R"([{"role":"system","content":"Recovered from crash"},{"role":"user","content":"Hello"}])";
    }

    MessageCompressionConfig cfg;
    cfg.auto_flush_enabled = false;
    Messages msgs(proj / ".hive-agent" / "default", cfg);
    CHECK(msgs.load());
    CHECK(msgs.size() == 2);
    CHECK(msgs.get_messages()[1]["content"] == "Hello");

    cleanup_test_project(proj);
}

TEST_CASE("CRASH-5 load falls back to .crash when messages.json corrupt") {
    auto proj = create_test_project();

    // Plant a corrupt messages.json.
    {
        std::ofstream f(proj / ".hive-agent" / "default" / "messages.json", std::ios::binary);
        f << "{ this is not valid JSON at all";
    }
    // Plant a valid .crash.
    {
        std::ofstream f(proj / ".hive-agent" / "default" / "messages.json.crash", std::ios::binary);
        f << R"([{"role":"system","content":"Recovered"}])";
    }

    MessageCompressionConfig cfg;
    cfg.auto_flush_enabled = false;
    Messages msgs(proj / ".hive-agent" / "default", cfg);
    CHECK(msgs.load());
    CHECK(msgs.size() == 1);

    cleanup_test_project(proj);
}

TEST_CASE("CRASH-5 load drops corrupt last message and re-saves") {
    auto proj = create_test_project();

    // Two valid messages + one corrupt last message (malformed tool
    // result — missing tool_call_id, classic cause of parser crash).
    {
        std::ofstream f(proj / ".hive-agent" / "default" / "messages.json.crash", std::ios::binary);
        f << R"([
            {"role":"system","content":"System"},
            {"role":"user","content":"Hello"},
            {"role":"tool","content":"orphaned result without tool_call_id"}
        ])";
    }

    MessageCompressionConfig cfg;
    cfg.auto_flush_enabled = false;
    Messages msgs(proj / ".hive-agent" / "default", cfg);
    CHECK(msgs.load());
    // Last message should have been dropped.
    CHECK(msgs.size() == 2);
    CHECK(msgs.get_messages().back()["content"] == "Hello");

    // messages.json should now contain the recovered chain so the
    // next launch goes through the normal load path.
    CHECK(std::filesystem::exists(proj / ".hive-agent" / "default" / "messages.json"));

    cleanup_test_project(proj);
}

TEST_CASE("CRASH-5 load keeps messages when last is well-formed") {
    auto proj = create_test_project();
    {
        std::ofstream f(proj / ".hive-agent" / "default" / "messages.json.crash", std::ios::binary);
        f << R"([
            {"role":"system","content":"System"},
            {"role":"user","content":"Hello"}
        ])";
    }
    MessageCompressionConfig cfg;
    cfg.auto_flush_enabled = false;
    Messages msgs(proj / ".hive-agent" / "default", cfg);
    CHECK(msgs.load());
    // Both messages kept (last is well-formed).
    CHECK(msgs.size() == 2);

    cleanup_test_project(proj);
}

TEST_CASE("CRASH-5 load returns false when no messages.json AND no .crash") {
    auto proj = create_test_project();
    MessageCompressionConfig cfg;
    cfg.auto_flush_enabled = false;
    Messages msgs(proj / ".hive-agent" / "default", cfg);
    CHECK_FALSE(msgs.load());
    CHECK(msgs.size() == 0);

    cleanup_test_project(proj);
}

// ============================================================================
// Test Suite: Tool Call State Machine
// ============================================================================

TEST_SUITE("Tool Call State Machine") {
    
    TEST_CASE("get_tool_state returns NOT_FOUND for unknown ID") {
        auto proj = create_test_project();
        auto msgs = create_test_messages(proj);
        
        msgs.add_system("You are helpful.");
        msgs.add_assistant("");
        msgs.get_last_assistant_message()["tool_calls"] = json::array({
            {{"id", "call_123"}, {"function", {{"name", "test"}, {"arguments", "{}"}}}}
        });
        
        CHECK(msgs.get_tool_state("nonexistent") == Messages::ToolState::INVALID);
        CHECK(msgs.get_tool_state("call_123") == Messages::ToolState::PENDING);
        
        cleanup_test_project(proj);
    }
    
    TEST_CASE("mark_tool_executing transitions PENDING to EXECUTING") {
        auto proj = create_test_project();
        auto msgs = create_test_messages(proj);
        
        msgs.add_system("You are helpful.");
        msgs.add_assistant("");
        msgs.get_last_assistant_message()["tool_calls"] = json::array({
            {{"id", "call_123"}, {"function", {{"name", "test"}, {"arguments", "{}"}}}}
        });
        
        // Before marking - PENDING
        CHECK(msgs.get_tool_state("call_123") == Messages::ToolState::PENDING);
        
        // Mark as executing
        bool result = msgs.mark_tool_executing("call_123");
        CHECK(result == true);
        
        // After marking - EXECUTING
        CHECK(msgs.get_tool_state("call_123") == Messages::ToolState::EXECUTING);
        CHECK(msgs.is_executing("call_123") == true);
        
        cleanup_test_project(proj);
    }
    
    TEST_CASE("mark_tool_executing fails for non-existent ID") {
        auto proj = create_test_project();
        auto msgs = create_test_messages(proj);
        
        bool result = msgs.mark_tool_executing("nonexistent");
        CHECK(result == false);
        
        cleanup_test_project(proj);
    }
    
    TEST_CASE("mark_tool_executing fails when not in PENDING state") {
        auto proj = create_test_project();
        auto msgs = create_test_messages(proj);
        
        msgs.add_system("You are helpful.");
        msgs.add_assistant("");
        msgs.get_last_assistant_message()["tool_calls"] = json::array({
            {{"id", "call_123"}, {"function", {{"name", "test"}, {"arguments", "{}"}}}}
        });
        
        // Mark as executing
        msgs.mark_tool_executing("call_123");
        
        // Try to mark again - should fail
        bool result = msgs.mark_tool_executing("call_123");
        CHECK(result == false);
        
        cleanup_test_project(proj);
    }
    
    TEST_CASE("add_tool_result transitions to COMPLETED (non-error)") {
        auto proj = create_test_project();
        auto msgs = create_test_messages(proj);
        
        msgs.add_system("You are helpful.");
        msgs.add_assistant("");
        msgs.get_last_assistant_message()["tool_calls"] = json::array({
            {{"id", "call_123"}, {"function", {{"name", "test"}, {"arguments", "{}"}}}}
        });
        
        msgs.mark_tool_executing("call_123");
        CHECK(msgs.get_tool_state("call_123") == Messages::ToolState::EXECUTING);
        
        // Add tool result (non-error)
        msgs.add_tool_result("call_123", "File content here");
        
        // In real LLMClient flow, clear_executing() is called after add_tool_result()
        // Here we simulate that by clearing the executing marker
        msgs.clear_executing("call_123");
        
        // After result - should be COMPLETED
        CHECK(msgs.get_tool_state("call_123") == Messages::ToolState::COMPLETED);
        
        cleanup_test_project(proj);
    }
    
    TEST_CASE("add_tool_result with Error: prefix transitions to FAILED") {
        auto proj = create_test_project();
        auto msgs = create_test_messages(proj);
        
        msgs.add_system("You are helpful.");
        msgs.add_assistant("");
        msgs.get_last_assistant_message()["tool_calls"] = json::array({
            {{"id", "call_err"}, {"function", {{"name", "test"}, {"arguments", "{}"}}}}
        });
        
        // Add error result with "Error:" prefix
        msgs.add_tool_result("call_err", "Error: something failed");
        
        // After error result - should be FAILED
        CHECK(msgs.get_tool_state("call_err") == Messages::ToolState::FAILED);
        
        cleanup_test_project(proj);
    }
    
    TEST_CASE("clear_executing removes EXECUTING state") {
        auto proj = create_test_project();
        auto msgs = create_test_messages(proj);
        
        msgs.add_system("You are helpful.");
        msgs.add_assistant("");
        msgs.get_last_assistant_message()["tool_calls"] = json::array({
            {{"id", "call_123"}, {"function", {{"name", "test"}, {"arguments", "{}"}}}}
        });
        
        msgs.mark_tool_executing("call_123");
        CHECK(msgs.is_executing("call_123") == true);
        
        msgs.clear_executing("call_123");
        CHECK(msgs.is_executing("call_123") == false);
        CHECK(msgs.get_tool_state("call_123") == Messages::ToolState::PENDING);
        
        cleanup_test_project(proj);
    }
    
    TEST_CASE("get_pending_tool_ids returns correct list") {
        auto proj = create_test_project();
        auto msgs = create_test_messages(proj);
        
        msgs.add_system("You are helpful.");
        msgs.add_assistant("");
        msgs.get_last_assistant_message()["tool_calls"] = json::array({
            {{"id", "call_1"}, {"function", {{"name", "test1"}, {"arguments", "{}"}}}},
            {{"id", "call_2"}, {"function", {{"name", "test2"}, {"arguments", "{}"}}}}
        });
        
        auto pending = msgs.get_pending_tool_ids();
        CHECK(pending.size() == 2);
        
        // Complete one tool
        msgs.add_tool_result("call_1", "result1");
        pending = msgs.get_pending_tool_ids();
        CHECK(pending.size() == 1);
        CHECK(pending[0] == "call_2");
        
        cleanup_test_project(proj);
    }
    
    TEST_CASE("has_pending_tools and has_executing_tools work correctly") {
        auto proj = create_test_project();
        auto msgs = create_test_messages(proj);
        
        msgs.add_system("You are helpful.");
        msgs.add_assistant("");
        msgs.get_last_assistant_message()["tool_calls"] = json::array({
            {{"id", "call_1"}, {"function", {{"name", "test"}, {"arguments", "{}"}}}}
        });
        
        // Before any action - call_1 is PENDING
        CHECK(msgs.has_pending_tools() == true);  // call_1 is PENDING
        CHECK(msgs.has_executing_tools() == false);
        
        msgs.mark_tool_executing("call_1");
        // After marking, call_1 is EXECUTING
        // Note: has_pending_tools checks if tools have NO result yet
        // A tool in EXECUTING still has no result, so it's still "pending" conceptually
        // but the EXECUTING state takes precedence in get_tool_state()
        CHECK(msgs.has_executing_tools() == true);
        
        // Simulate LLMClient clearing executing after adding result
        msgs.clear_executing("call_1");
        msgs.add_tool_result("call_1", "done");
        CHECK(msgs.has_executing_tools() == false);
        CHECK(msgs.has_pending_tools() == false);  // call_1 now COMPLETED
        
        cleanup_test_project(proj);
    }
    
    TEST_CASE("all_tools_resolved returns correct value") {
        auto proj = create_test_project();
        auto msgs = create_test_messages(proj);
        
        msgs.add_system("You are helpful.");
        msgs.add_assistant("");
        msgs.get_last_assistant_message()["tool_calls"] = json::array({
            {{"id", "call_1"}, {"function", {{"name", "test"}, {"arguments", "{}"}}}}
        });
        
        CHECK(msgs.all_tools_resolved() == false);
        
        msgs.add_tool_result("call_1", "done");
        CHECK(msgs.all_tools_resolved() == true);
        
        cleanup_test_project(proj);
    }
    
    TEST_CASE("clear_executing_markers clears all executing tools") {
        auto proj = create_test_project();
        auto msgs = create_test_messages(proj);
        
        msgs.add_system("You are helpful.");
        msgs.add_assistant("");
        msgs.get_last_assistant_message()["tool_calls"] = json::array({
            {{"id", "call_1"}, {"function", {{"name", "test1"}, {"arguments", "{}"}}}},
            {{"id", "call_2"}, {"function", {{"name", "test2"}, {"arguments", "{}"}}}}
        });
        
        msgs.mark_tool_executing("call_1");
        msgs.mark_tool_executing("call_2");
        CHECK(msgs.has_executing_tools() == true);
        
        msgs.clear_executing_markers();
        CHECK(msgs.has_executing_tools() == false);
        
        cleanup_test_project(proj);
    }
    
    TEST_CASE("Tool state persists correctly after save/load") {
        auto proj = create_test_project();
        
        // Create messages with completed and failed tools
        {
            auto msgs = create_test_messages(proj);
            msgs.add_system("You are helpful.");
            msgs.add_assistant("");
            msgs.get_last_assistant_message()["tool_calls"] = json::array({
                {{"id", "call_success"}, {"function", {{"name", "test"}, {"arguments", "{}"}}}},
                {{"id", "call_failed"}, {"function", {{"name", "test"}, {"arguments", "{}"}}}}
            });
            
            msgs.add_tool_result("call_success", "success result");
            msgs.add_tool_result("call_failed", "Error: failed result");
            
            msgs.save();
        }
        
        // Load and verify state persisted
        auto msgs2 = create_test_messages(proj);
        CHECK(msgs2.load() == true);
        
        // Note: EXECUTING state is ephemeral and won't persist
        // But COMPLETED and FAILED states derive from message chain
        CHECK(msgs2.get_tool_state("call_success") == Messages::ToolState::COMPLETED);
        CHECK(msgs2.get_tool_state("call_failed") == Messages::ToolState::FAILED);
        
        cleanup_test_project(proj);
    }
    
    TEST_CASE("has_tool_result works correctly") {
        auto proj = create_test_project();
        auto msgs = create_test_messages(proj);
        
        msgs.add_system("You are helpful.");
        msgs.add_assistant("");
        msgs.get_last_assistant_message()["tool_calls"] = json::array({
            {{"id", "call_1"}, {"function", {{"name", "test"}, {"arguments", "{}"}}}}
        });
        
        // Manually add tool result (bypass normal flow)
        msgs.get_messages().push_back({
            {"role", "tool"},
            {"tool_call_id", "call_1"},
            {"content", "result"}
        });
        
        // Check via get_tool_state
        CHECK(msgs.get_tool_state("call_1") == Messages::ToolState::COMPLETED);
        
        cleanup_test_project(proj);
    }
}

// ============================================================================
// Test Suite: Compression Regression - Safe Split with Tool Calls
// ============================================================================
// These tests are REGRESSION tests for a critical bug where the safe split
// logic was too aggressive. When a tool call pair was at the beginning of
// messages, the safe split would shrink to 1, deleting only 1 message
// instead of ~70%.
//
// The fix: the algorithm should EXTEND the target split to AFTER the tool
// result (so the whole tool call chain is in the compressed region), not
// shrink it to before the assistant.

TEST_SUITE("Compression Regression - Safe Split with Tool Calls") {

    /**
     * Helper: Create a tool call message (assistant with tool_calls)
     */
    json make_tool_call_msg(const std::string& tc_id, const std::string& tool_name) {
        return {
            {"role", "assistant"},
            {"tool_calls", json::array({
                {
                    {"id", tc_id},
                    {"type", "function"},
                    {"function", {
                        {"name", tool_name},
                        {"arguments", "{}"}
                    }}
                }
            })}
        };
    }

    /**
     * Helper: Create a tool result message
     */
    json make_tool_result_msg(const std::string& tc_id, const std::string& content) {
        return {
            {"role", "tool"},
            {"tool_call_id", tc_id},
            {"content", content}
        };
    }

    /**
     * REGRESSION TEST 1: Tool call pair at the very beginning
     * This was the exact bug from prompt.txt - 595 messages, first message
     * was a user prompt, second was assistant with git_add tool_calls,
     * third was tool result. The old algorithm would shrink the split
     * to 1, deleting only the first message.
     *
     * Expected: The compression should still delete ~70% of messages
     * (with the whole tool call chain in the compressed region).
     */
    TEST_CASE("Tool call pair at the very beginning - should still compress ~70%") {
        auto proj = create_test_project();
        auto msgs = create_test_messages(proj);

        // Build 100 messages: system + 99 non-system
        // First 10 messages contain a tool call pair (simulating the bug scenario)
        json test_msgs = json::array();
        test_msgs.push_back(make_msg("system", "System prompt"));

        // Add 5 user/assistant pairs (10 messages) with a tool call in the middle
        for (int i = 0; i < 5; ++i) {
            test_msgs.push_back(make_msg("user", "Task " + std::to_string(i)));
            test_msgs.push_back(make_msg("assistant", "Response " + std::to_string(i)));
        }
        // Add a tool call pair (assistant with tool_calls + tool result)
        test_msgs.push_back(make_tool_call_msg("call_001", "git_add"));
        test_msgs.push_back(make_tool_result_msg("call_001", "Files staged successfully"));

        // Add 88 more user/assistant pairs (44 pairs = 88 messages) to reach 100 non-system
        // Total: 1 system + 10 (5 pairs) + 2 (tool call pair) + 88 (44 pairs) = 101
        for (int i = 0; i < 44; ++i) {
            test_msgs.push_back(make_msg("user", "Task " + std::to_string(i + 5)));
            test_msgs.push_back(make_msg("assistant", "Response " + std::to_string(i + 5)));
        }

        msgs.set_messages(test_msgs);
        REQUIRE(msgs.size() == 101);  // 1 system + 100 non-system

        // 70% of 100 = 70, so middle should be ~70, suffix ~30
        // The tool call pair is at indices 12-13 (after 10 normal + 2 from pair)
        // Since the tool call pair is in [1, 70], it should be INCLUDED in compression
        size_t prefix_size, middle_size, suffix_size;
        msgs.get_split_info(prefix_size, middle_size, suffix_size);

        // Critical: middle should be ~70 (NOT 1!)
        CHECK(middle_size >= 60);  // At least 60 (allowing some flexibility)
        CHECK(middle_size <= 72);  // At most 72
        CHECK(suffix_size >= 28);  // At least 28 kept
        CHECK(suffix_size <= 40);  // At most 40 kept

        // Verify the compression indices
        auto [start, end] = msgs.get_compression_indices();
        CHECK(start == 1);
        CHECK(end >= 61);  // Should be at least 61 (not 2!)

        cleanup_test_project(proj);
    }

    /**
     * REGRESSION TEST 2: get_compression_indices with tool call at start
     * Direct test of the get_compression_indices() function which is
     * called by compress(). The old buggy version returned (1, 2) when
     * there was a tool call at index 1-2.
     */
    TEST_CASE("get_compression_indices does not shrink to 1 with tool call at start") {
        auto proj = create_test_project();
        auto msgs = create_test_messages(proj);

        // Build: system + user + assistant(tool_calls) + tool_result + 96 more
        json test_msgs = json::array();
        test_msgs.push_back(make_msg("system", "System prompt"));
        test_msgs.push_back(make_msg("user", "First user message"));
        test_msgs.push_back(make_tool_call_msg("call_abc", "some_tool"));
        test_msgs.push_back(make_tool_result_msg("call_abc", "Some result"));

        // Add 97 more messages (48 pairs = 96, +1 extra = 97)
        // Total: 1 system + 1 user + 2 (tool call pair) + 97 = 101
        for (int i = 0; i < 48; ++i) {
            test_msgs.push_back(make_msg("user", "User " + std::to_string(i)));
            test_msgs.push_back(make_msg("assistant", "Assistant " + std::to_string(i)));
        }
        test_msgs.push_back(make_msg("user", "Extra user"));

        msgs.set_messages(test_msgs);
        REQUIRE(msgs.size() == 101);  // 1 system + 100 non-system

        auto [start, end] = msgs.get_compression_indices();

        // CRITICAL: The old bug returned (1, 2) - deleting only 1 message
        // The fix: end should be ~71 (70% of 100 = 70, plus 1 for inclusive end)
        CHECK(start == 1);
        CHECK(end >= 50);   // At least 50 (definitely NOT 2!)
        CHECK(end <= 75);   // At most 75

        cleanup_test_project(proj);
    }

    /**
     * REGRESSION TEST 3: Many tool call pairs throughout
     * Real-world scenario: 595 messages with many tool call pairs scattered
     * throughout. The algorithm should still compress ~70%.
     */
    TEST_CASE("Many tool call pairs throughout - still compresses ~70%") {
        auto proj = create_test_project();
        auto msgs = create_test_messages(proj);

        json test_msgs = json::array();
        test_msgs.push_back(make_msg("system", "System prompt"));

        // 100 non-system messages with tool call pairs scattered
        for (int i = 0; i < 100; ++i) {
            if (i % 5 == 0 && i < 95) {
                // Add a tool call pair every 5 messages
                std::string tc_id = "call_" + std::to_string(i);
                test_msgs.push_back(make_tool_call_msg(tc_id, "tool_" + std::to_string(i)));
                test_msgs.push_back(make_tool_result_msg(tc_id, "result " + std::to_string(i)));
            } else {
                test_msgs.push_back(make_msg("user", "User " + std::to_string(i)));
                test_msgs.push_back(make_msg("assistant", "Assistant " + std::to_string(i)));
            }
        }

        msgs.set_messages(test_msgs);
        // Size will be: 1 system + (some messages based on loop logic)
        // Just verify the split is reasonable
        size_t prefix_size, middle_size, suffix_size;
        msgs.get_split_info(prefix_size, middle_size, suffix_size);

        // Verify middle is ~70% and suffix is ~30%
        size_t total_non_system = msgs.size() - 1;
        size_t expected_middle = (total_non_system * 70) / 100;
        size_t expected_suffix = total_non_system - expected_middle;

        // Allow some flexibility for the safe split adjustments
        CHECK(middle_size >= expected_middle - 5);
        CHECK(middle_size <= expected_middle + 5);
        CHECK(suffix_size >= expected_suffix - 5);
        CHECK(suffix_size <= expected_suffix + 5);

        cleanup_test_project(proj);
    }

    /**
     * REGRESSION TEST 4: Tool call pair straddling the 70% boundary
     * A tool call pair that starts in the first 70% and the result
     * is in the last 30%. The whole chain should be in the compressed region.
     */
    TEST_CASE("Tool call pair straddling the 70% boundary - whole chain compressed") {
        auto proj = create_test_project();
        auto msgs = create_test_messages(proj);

        json test_msgs = json::array();
        test_msgs.push_back(make_msg("system", "System prompt"));

        // 69 normal messages, then a tool call (assistant) at index 70,
        // then tool result at index 71, then more messages
        for (int i = 0; i < 69; ++i) {
            test_msgs.push_back(make_msg("user", "User " + std::to_string(i)));
        }
        // Tool call at index 70 (just inside the 70% boundary)
        test_msgs.push_back(make_tool_call_msg("call_boundary", "straddle_tool"));
        // Tool result at index 71 (just outside the 70% boundary)
        test_msgs.push_back(make_tool_result_msg("call_boundary", "result"));

        // Add 29 more messages (14 pairs = 28, +1 extra = 29)
        // Total: 1 system + 69 user + 2 (tool call pair) + 29 = 101
        for (int i = 0; i < 14; ++i) {
            test_msgs.push_back(make_msg("user", "Tail user " + std::to_string(i)));
            test_msgs.push_back(make_msg("assistant", "Tail assistant " + std::to_string(i)));
        }
        test_msgs.push_back(make_msg("user", "Extra tail user"));

        msgs.set_messages(test_msgs);
        REQUIRE(msgs.size() == 101);

        auto [start, end] = msgs.get_compression_indices();

        // The tool call pair is at indices 70-71 (0-indexed) = 71-72 (1-indexed)
        // The whole chain should be in the compressed region, so end should be >= 72
        CHECK(start == 1);
        CHECK(end >= 72);  // Must include the tool result

        cleanup_test_project(proj);
    }

    /**
     * REGRESSION TEST 5: No tool calls - basic 70/30 split
     * Sanity check: with no tool calls, the split should be exactly 70/30.
     */
    TEST_CASE("No tool calls - basic 70/30 split") {
        auto proj = create_test_project();
        auto msgs = create_test_messages(proj);

        json test_msgs = json::array();
        test_msgs.push_back(make_msg("system", "System prompt"));

        for (int i = 0; i < 100; ++i) {
            test_msgs.push_back(make_msg("user", "User " + std::to_string(i)));
        }

        msgs.set_messages(test_msgs);
        REQUIRE(msgs.size() == 101);

        auto [start, end] = msgs.get_compression_indices();

        // 70% of 100 = 70, so end = 1 + 70 = 71
        CHECK(start == 1);
        CHECK(end == 71);

        cleanup_test_project(proj);
    }

    /**
     * REGRESSION TEST 6: All tool calls in first 70%
     * All tool call pairs are in the compressed region. The split
     * should still be ~70% with all chains included.
     */
    TEST_CASE("All tool calls in first 70% - whole chains compressed") {
        auto proj = create_test_project();
        auto msgs = create_test_messages(proj);

        json test_msgs = json::array();
        test_msgs.push_back(make_msg("system", "System prompt"));

        // Add 10 tool call pairs (20 messages) at the beginning
        for (int i = 0; i < 10; ++i) {
            std::string tc_id = "call_" + std::to_string(i);
            test_msgs.push_back(make_tool_call_msg(tc_id, "tool_" + std::to_string(i)));
            test_msgs.push_back(make_tool_result_msg(tc_id, "result " + std::to_string(i)));
        }
        // Add 80 normal messages
        for (int i = 0; i < 80; ++i) {
            test_msgs.push_back(make_msg("user", "User " + std::to_string(i)));
        }

        msgs.set_messages(test_msgs);
        REQUIRE(msgs.size() == 101);

        auto [start, end] = msgs.get_compression_indices();

        // 70% of 100 = 70, so end should be ~71
        // All 20 tool call messages are in the first 20, well within 70%
        CHECK(start == 1);
        CHECK(end >= 70);
        CHECK(end <= 72);

        cleanup_test_project(proj);
    }
}

// ===========================================================================
// §B5 regression test: truncate_if_needed preserves the tool-call chain.
//
// Pre-fix, the inner `while (i >= 1)` block read `prev_role` from
// messages_[i] BEFORE pushing, then checked it AFTER pushing. On
// multi-step chains where the parent assistant sits more than one
// position back, the walk stopped at the wrong index and emitted a
// truncated chain with no tool_calls parent — the very next LLM
// call returned 400 "tool call result does not follow tool call".
//
// This test sets up a chain of [assistant(tool_call), tool_result,
// assistant(tool_call), tool_result] where the truncation point lands
// between the two pairs (so the second tool_result's parent assistant
// is at index >= 2 before the truncation walks past it). The new
// code finds the parent assistant and pulls in the chain intact.
// ===========================================================================
TEST_CASE("Messages - truncate_if_needed preserves multi-step tool chain (B5)") {
    fs::path proj = create_test_project();
    // PERSIST-DIRTY (2026-08-31): disable the background flush worker.
    // set_messages() now marks the chain dirty; with the default config
    // (auto-flush enabled) the worker saves concurrently and races the
    // remove_all() in cleanup_test_project ("Directory not empty").
    MessageCompressionConfig cfg;
    cfg.auto_flush_enabled = false;
    Messages msgs(proj, cfg);
    msgs.load();  // creates messages.json with []

    // Build a chain that totals > 2000 bytes. Each tool result has
    // enough padding to push us over the limit. Tool ids are "A1" and
    // "B2" — the parent assistant must be present in the truncated
    // chain for both, or the next LLM call fails with 400.
    json test_msgs = json::array();
    test_msgs.push_back({{"role", "user"},   {"content", "do step 1"}});
    test_msgs.push_back({
        {"role", "assistant"}, {"content", ""},
        {"tool_calls", json::array({
            {{"id", "A1"}, {"type", "function"},
             {"function", {{"name", "do_tool"}, {"arguments", "{}"}}}}
        })}});
    std::string padding_a(800, 'x');
    test_msgs.push_back({
        {"role", "tool"}, {"tool_call_id", "A1"},
        {"content", "tool-msg-A1-" + padding_a}});
    test_msgs.push_back({{"role", "user"},   {"content", "ok now do step 2"}});
    test_msgs.push_back({
        {"role", "assistant"}, {"content", ""},
        {"tool_calls", json::array({
            {{"id", "B2"}, {"type", "function"},
             {"function", {{"name", "do_tool"}, {"arguments", "{}"}}}}
        })}});
    std::string padding_b(800, 'y');
    test_msgs.push_back({
        {"role", "tool"}, {"tool_call_id", "B2"},
        {"content", "tool-msg-B2-" + padding_b}});
    test_msgs.push_back({{"role", "user"},   {"content", "thanks"}});
    msgs.set_messages(test_msgs);

    auto result = msgs.truncate_if_needed(2000);
    CHECK(result.value("truncated", false) == true);

    auto& arr = msgs.get_messages();
    for (size_t i = 0; i < arr.size(); ++i) {
        if (arr[i].value("role", "") != "tool") continue;
        std::string tc_id = arr[i].value("tool_call_id", "");
        INFO("Checking tool result at index " << i
             << " (id='" << tc_id << "')");
        // Walk backward looking for an assistant with matching id.
        bool found = false;
        for (size_t j = i; j-- > 0; ) {
            if (arr[j].value("role", "") != "assistant") continue;
            if (!arr[j].contains("tool_calls")) continue;
            for (const auto& tc : arr[j]["tool_calls"]) {
                if (tc.value("id", "") == tc_id) { found = true; break; }
            }
            if (found) break;
        }
        CHECK(found);
    }

    cleanup_test_project(proj);
}

// ===========================================================================
// §B8 regression test: inject_context_stats does not drop the trailing
// tail of the system prompt when the stats block is the LAST section.
//
// Pre-fix, the erase-from-`# Context Status`-to-next-heading logic
// searched for `\n# ` (or `\n## `) AFTER the stats block. When the stats
// block was the last section in the system prompt, no such heading
// existed; the code fell through to `context_end = content.size()`
// and `content.erase(ctx_start, content.size() - ctx_start)` —
// wiping everything after the stats block, including the parts of
// the system prompt that came after it.
// ===========================================================================
TEST_CASE("Messages - inject_context_stats preserves trailing prompt (B8)") {
    fs::path proj = create_test_project();
    Messages msgs(proj, {});

    // System prompt where the Context Status block is the LAST section.
    // (No H1/H2 heading follows it.)
    std::string system_with_stats_last =
        "# Role\n"
        "You are a helpful assistant.\n"
        "\n"
        "## Context Status\n"
        "- **Token Usage**: 100/1000 (10%)\n"
        "- **Iterations**: 1/100\n"
        "- **Messages**: 3\n"
        "<!-- /hive-agent-context-status -->\n"   // include sentinel for clean
                                                // first-time write
        "\n"
        "## Closing Notes\n"
        "Always cite your sources.\n";           // ← would be wiped by the bug

    // Pre-fix blocks # Context Status body would be replaced
    // wholesale; # Closing Notes would survive the H2 search but only
    // by accident (it has the sentinel-friendly pattern). With the
    // sentinel fix, we erase EXACTLY the prior sentinel block, leaving
    // everything else intact.
    msgs.get_messages().push_back({
        {"role", "system"},
        {"content", system_with_stats_last}});
    msgs.get_messages().push_back({{"role", "user"}, {"content", "hi"}});
    msgs.get_messages().push_back({{"role", "assistant"}, {"content", "hello"}});

    // Run inject (uses the stats config).
    msgs.inject_context_stats(500, 2, 100);

    // Pull the post-injection content out and verify the trailing
    // "Closing Notes" survived.
    auto& arr = msgs.get_messages();
    std::string post = arr[0].value("content", "");

    CHECK(post.find("# Role") != std::string::npos);
    CHECK(post.find("You are a helpful assistant") != std::string::npos);
    CHECK(post.find("## Closing Notes") != std::string::npos);
    CHECK(post.find("Always cite your sources") != std::string::npos);

    // Exactly one sentinel line — not two (which would mean a stale
    // pre-fix block was left behind).
    size_t s1 = post.find("<!-- /hive-agent-context-status -->");
    REQUIRE(s1 != std::string::npos);
    size_t s2 = post.find("<!-- /hive-agent-context-status -->", s1 + 1);
    CHECK(s2 == std::string::npos);

    cleanup_test_project(proj);
}

// ============================================================================
// PERSIST-DIRTY regression tests (2026-08-31): shutdown/startup "last
// message re-sent" report. Simulates session 1 saving on close, session 2
// loading and sending a NEW message. Asserts no duplication and a sane
// wire-chain tail.
// ============================================================================
TEST_CASE("Messages - reload after clean shutdown does not duplicate last message") {
    auto proj = create_test_project();

    // ---- Session 1: full exchange + trailing transient status, then close.
    {
        auto msgs = create_test_messages(proj);
        msgs.add_system("sys prompt");
        msgs.add_user("user question from session 1");
        msgs.add_assistant("assistant answer from session 1");
        // inject_context_stats() leaves this transient trailing system
        // message in the live chain; cleanup()->save() persists it.
        msgs.get_messages().push_back({
            {"role", "system"},
            {"content", "<!-- hive-agent-context-status -->\n# Context Status\n| fake | 1 |\n"}
        });
        msgs.save();
    } // dtor (auto_flush disabled in tests: no worker)

    // ---- Session 2: startup load.
    auto msgs2 = create_test_messages(proj);
    REQUIRE(msgs2.load());

    // Count occurrences of each session-1 message - none may be duplicated.
    int n_sys = 0, n_u1 = 0, n_a1 = 0, n_status = 0;
    for (const auto& m : msgs2.get_messages()) {
        const std::string c = m.value("content", "");
        if (c == "sys prompt") ++n_sys;
        if (c.find("user question from session 1") != std::string::npos) ++n_u1;
        if (c.find("assistant answer from session 1") != std::string::npos) ++n_a1;
        if (c.find("hive-agent-context-status") != std::string::npos) ++n_status;
    }
    CHECK(n_sys == 1);
    CHECK(n_u1 == 1);
    CHECK(n_a1 == 1);

    // ---- Simulate LLMClient::run_one_turn("new user msg"):
    json& live = msgs2.get_messages();
    live.push_back({{"role", "user"}, {"content", "brand new question"}});

    // inject_context_stats equivalent: drop old status msgs, append fresh.
    auto& arr = live;
    for (auto it = arr.begin(); it != arr.end();) {
        if (it->value("role", "") == "system" &&
            it->value("content", "").find("<!-- hive-agent-context-status -->") != std::string::npos) {
            it = arr.erase(it);
        } else {
            ++it;
        }
    }
    arr.push_back({{"role","system"},{"content","<!-- hive-agent-context-status -->\n# Context Status\n| fresh | 2 |\n"}});

    n_u1 = 0; n_a1 = 0; n_status = 0;
    int n_u2 = 0;
    for (const auto& m : arr) {
        const std::string c = m.value("content", "");
        if (c.find("user question from session 1") != std::string::npos) ++n_u1;
        if (c.find("assistant answer from session 1") != std::string::npos) ++n_a1;
        if (c.find("brand new question") != std::string::npos) ++n_u2;
        if (c.find("hive-agent-context-status") != std::string::npos) ++n_status;
    }
    CHECK(n_u1 == 1);
    CHECK(n_a1 == 1);
    CHECK(n_u2 == 1);
    CHECK(n_status == 1);
    // Second-to-last must be the new user message (status is appended last).
    CHECK(arr[arr.size() - 2].value("content", "") == "brand new question");

    cleanup_test_project(proj);
}

// PERSIST-DIRTY: set_messages() now marks the chain dirty so the flush
// worker (or the destructor's final flush) persists rewrites made through
// the raw-reference path (sanitize/truncate/compress in LLMClient).
TEST_CASE("Messages - set_messages marks chain dirty (write-through restored)") {
    auto proj = create_test_project();

    MessageCompressionConfig cfg;
    cfg.auto_flush_enabled = true;
    cfg.auto_flush_interval_ms = 50;

    {
        Messages msgs(proj / ".hive-agent" / "default", cfg);
        // Route the initial content through set_messages only - no add_*().
        msgs.set_messages(json::array({
            {{"role", "system"},    {"content", "sys"}},
            {{"role", "user"},      {"content", "u1"}},
            {{"role", "assistant"}, {"content", "a1"}},
        }));
        msgs.flush_now();  // deterministic: wait for the worker-visible state
    } // dtor: worker's final flush_if_dirty() must have saved

    auto reload = create_test_messages(proj);
    REQUIRE(reload.load());
    CHECK(reload.size() == 3);
    CHECK(reload.get_messages()[2].value("content", "") == "a1");

    cleanup_test_project(proj);
}

// ============================================================================
// §PERSIST-RACE / SANITIZE-NOTHROW regression suite
// (daedalus_bugs.md BUG 1, MonkeyByte coredump 2026-08-30 + SIGSEGV 2026-09-02)
// ============================================================================

TEST_SUITE("PERSIST-RACE") {

    TEST_CASE("stress: raw get_messages() mutation + mark_dirty while flush worker saves") {
        // Simulates the LLMClient turn path: the chain is mutated through
        // the raw get_messages() reference (no mutex) and mark_dirty() is
        // called after each push, while the background flush worker saves
        // at a 1ms interval. Pre-fix the worker deep-copied messages_
        // concurrently with those unlocked push_backs -> torn json read ->
        // SIGSEGV in the host process. Post-fix the worker only ever reads
        // the published snapshot taken at mark time.
        auto proj = create_test_project();
        MessageCompressionConfig config;
        config.debug_output = false;
        config.auto_flush_enabled = true;
        config.auto_flush_interval_ms = 1;
        {
            Messages msgs(proj / ".hive-agent" / "default", config);
            for (int i = 0; i < 1000; ++i) {
                msgs.get_messages().push_back(make_msg(
                    i % 2 ? "assistant" : "user",
                    "message payload number " + std::to_string(i)
                        + " with some body text to give the copier work"));
                msgs.mark_dirty();
            }
            msgs.flush_now();
        }  // dtor: stop worker + final flush

        // Reload and verify the full chain survived to disk.
        MessageCompressionConfig cfg2;
        cfg2.auto_flush_enabled = false;
        cfg2.debug_output = false;
        Messages check(proj / ".hive-agent" / "default", cfg2);
        REQUIRE(check.load());
        CHECK(check.get_messages().size() == 1000);
        CHECK(check.get_messages().back()["content"].get<std::string>()
                  .find("999") != std::string::npos);
        cleanup_test_project(proj);
    }

    TEST_CASE("save() persists a raw-reference push without mark_dirty (legacy semantics)") {
        // Hosts/tests that push via get_messages() and then call save()
        // directly must still persist the live chain: public save()
        // refreshes the published snapshot before writing.
        auto proj = create_test_project();
        {
            auto msgs = create_test_messages(proj);
            msgs.get_messages().push_back(make_msg("system", "s"));
            msgs.get_messages().push_back(make_msg("user", "raw push, no mark_dirty"));
            msgs.save();
        }
        auto check = create_test_messages(proj);
        REQUIRE(check.load());
        REQUIRE(check.get_messages().size() == 2);
        CHECK(check.get_messages()[1]["content"] == "raw push, no mark_dirty");
        cleanup_test_project(proj);
    }
}

TEST_SUITE("SANITIZE-NOTHROW") {

    TEST_CASE("save() survives malformed chain entries without throwing") {
        // MonkeyByte coredump 2026-08-30: entry.value("role", ...) threw
        // type_error on non-object / null-role entries and escaped save()
        // onto the flush worker -> std::terminate -> SIGABRT. Sanitize
        // must drop unusable entries instead.
        auto proj = create_test_project();
        auto msgs = create_test_messages(proj);
        json chain = json::array();
        chain.push_back(make_msg("system", "s"));
        chain.push_back(make_msg("user", "hello"));
        chain.push_back("i am a bare string, not an object");
        chain.push_back(42);
        chain.push_back(nullptr);
        chain.push_back(json{{"role", nullptr}});
        chain.push_back(json{{"role", "tool"}, {"tool_call_id", nullptr},
                             {"content", "x"}});
        chain.push_back(json{{"role", "assistant"}, {"content", "a"},
                             {"tool_calls", json::array({"not-an-object"})}});
        chain.push_back(json{{"role", "assistant"},
                             {"tool_calls", json::array({{{"id", nullptr}}})}});
        msgs.set_messages(chain);

        CHECK_NOTHROW(msgs.save());

        auto check = create_test_messages(proj);
        REQUIRE(check.load());
        // Only structurally usable entries survive.
        for (const auto& m : check.get_messages()) {
            CHECK(m.is_object());
            CHECK(m.value("role", "") != "");
        }
        cleanup_test_project(proj);
    }

    TEST_CASE("cleanup_dangling_tool_results survives a non-object tail entry") {
        auto proj = create_test_project();
        auto msgs = create_test_messages(proj);
        json chain = json::array();
        chain.push_back(make_msg("system", "s"));
        chain.push_back(make_msg("user", "u"));
        chain.push_back("corrupt tail entry");
        msgs.set_messages(chain);
        CHECK_NOTHROW(msgs.cleanup_dangling_tool_results());
        cleanup_test_project(proj);
    }
}
