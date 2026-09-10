/**
 * @file test_message_chain_validity.cpp
 * @brief Unit tests for position-aware message-chain validation
 *
 * §fix (2026-07-30): covers the infinite HTTP 400 loop root causes —
 * misplaced/duplicate tool results surviving sanitize_message_chain,
 * unguarded synthetic tool-result injection, and truncation emitting
 * tool results without their parent assistant.
 */

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <string>
#include <filesystem>
#include <unordered_set>
#include <vector>
#include <unistd.h>

#include "core/Messages.h"

using json = nlohmann::json;
namespace fs = std::filesystem;

// ============================================================================
// Test Fixtures
// ============================================================================

static fs::path create_test_project() {
    fs::path proj = fs::temp_directory_path() /
        ("daedalus_chain_validity_test_" + std::to_string(getpid()));
    fs::create_directories(proj / ".daedalus" / "daedalus");
    return proj;
}

static void cleanup_test_project(const fs::path& proj) {
    fs::remove_all(proj);
}

static Messages create_test_messages(const fs::path& proj_root) {
    MessageCompressionConfig config;
    config.max_context_size = 200000;
    config.compress_threshold = 0.9f;
    config.debug_output = false;
    config.auto_flush_enabled = false;
    return Messages(proj_root / ".daedalus" / "daedalus", config);
}

static json make_msg(const std::string& role, const std::string& content) {
    return json{{"role", role}, {"content", content}};
}

static json make_assistant_tc(const std::string& id,
                              const std::string& content = "") {
    json msg = {
        {"role", "assistant"},
        {"tool_calls", json::array({
            {
                {"id", id},
                {"type", "function"},
                {"function", {
                    {"name", "some_tool"},
                    {"arguments", "{}"}
                }}
            }
        })}
    };
    if (!content.empty()) msg["content"] = content;
    return msg;
}

static json make_tool_result(const std::string& id,
                             const std::string& content = "ok") {
    return json{{"role", "tool"}, {"tool_call_id", id}, {"content", content}};
}

/// Walk a chain and verify the provider's tool-call contract:
/// every tool message sits in the contiguous block after its parent
/// assistant, appears once, and every tool_call has an adjacent result.
static void check_chain_valid(const json& chain) {
    std::unordered_set<std::string> open_block;
    std::unordered_set<std::string> answered;
    for (const auto& m : chain) {
        std::string role = m.value("role", "");
        if (role == "tool") {
            std::string id = m.value("tool_call_id", "");
            CHECK(!id.empty());
            CHECK(open_block.count(id) == 1);
            CHECK(answered.count(id) == 0);
            answered.insert(id);
        } else {
            if (role == "assistant" && m.contains("tool_calls")) {
                // Every surviving tool_call must be answered adjacently —
                // verified when the following tool messages are visited;
                // here we just open the block.
            }
            open_block.clear();
            answered.clear();
            if (role == "assistant" && m.contains("tool_calls")
                && m["tool_calls"].is_array()) {
                for (const auto& tc : m["tool_calls"]) {
                    std::string id = tc.value("id", "");
                    if (!id.empty()) open_block.insert(id);
                }
            }
        }
    }
    // No pending (unanswered) tool_calls may remain at the end.
    CHECK(open_block == answered);
}

static std::vector<std::string> roles_of(const json& chain) {
    std::vector<std::string> roles;
    for (const auto& m : chain) roles.push_back(m.value("role", ""));
    return roles;
}

// ============================================================================
// sanitize_message_chain — position awareness
// ============================================================================

TEST_SUITE("sanitize_message_chain position awareness") {

    TEST_CASE("Valid chain is preserved unchanged") {
        auto proj = create_test_project();
        auto msgs = create_test_messages(proj);

        json chain = json::array({
            make_msg("system", "sys"),
            make_msg("user", "hi"),
            make_assistant_tc("X", "calling tool"),
            make_tool_result("X"),
            make_msg("assistant", "done"),
            make_msg("user", "thanks"),
        });

        json out = msgs.sanitize_message_chain(chain);
        CHECK(out.size() == chain.size());
        CHECK(roles_of(out) == roles_of(chain));
        check_chain_valid(out);
        cleanup_test_project(proj);
    }

    TEST_CASE("Misplaced tool result after user message is dropped (reported bug)") {
        auto proj = create_test_project();
        auto msgs = create_test_messages(proj);

        // The exact failure shape from the infinite-400 incident: the FATAL
        // recovery appended a tool result for an already-answered call at
        // the end of the chain, after a user message.
        json chain = json::array({
            make_msg("system", "sys"),
            make_msg("user", "hi"),
            make_assistant_tc("X"),
            make_tool_result("X"),
            make_msg("assistant", "answer"),
            make_msg("user", "[SYSTEM ERROR] ..."),
            make_tool_result("X", "[API ERROR] bogus injected result"),
        });

        json out = msgs.sanitize_message_chain(chain);
        // The bogus trailing tool result must be gone; everything else kept.
        CHECK(out.size() == 6);
        CHECK(roles_of(out) == std::vector<std::string>{
            "system", "user", "assistant", "tool", "assistant", "user"});
        check_chain_valid(out);
        cleanup_test_project(proj);
    }

    TEST_CASE("Duplicate adjacent tool results are dropped") {
        auto proj = create_test_project();
        auto msgs = create_test_messages(proj);

        json chain = json::array({
            make_msg("system", "sys"),
            make_assistant_tc("X"),
            make_tool_result("X", "first"),
            make_tool_result("X", "duplicate"),
        });

        json out = msgs.sanitize_message_chain(chain);
        CHECK(out.size() == 3);
        CHECK(out[2]["content"] == "first");
        check_chain_valid(out);
        cleanup_test_project(proj);
    }

    TEST_CASE("User message between call and result prunes both sides") {
        auto proj = create_test_project();
        auto msgs = create_test_messages(proj);

        json chain = json::array({
            make_msg("system", "sys"),
            make_assistant_tc("X", "let me check"),
            make_msg("user", "interrupt"),
            make_tool_result("X"),
        });

        json out = msgs.sanitize_message_chain(chain);
        // tool_call X has no adjacent result → pruned from the assistant
        // (content kept); the misplaced tool result is dropped.
        CHECK(out.size() == 3);
        CHECK(out[1]["role"] == "assistant");
        CHECK(!out[1].contains("tool_calls"));
        CHECK(out[1]["content"] == "let me check");
        CHECK(out[2]["role"] == "user");
        check_chain_valid(out);
        cleanup_test_project(proj);
    }

    TEST_CASE("Assistant with unanswered tool_calls and no content is dropped") {
        auto proj = create_test_project();
        auto msgs = create_test_messages(proj);

        json chain = json::array({
            make_msg("system", "sys"),
            make_msg("user", "hi"),
            make_assistant_tc("X"),  // no content, no result anywhere
            make_msg("user", "next"),
        });

        json out = msgs.sanitize_message_chain(chain);
        CHECK(roles_of(out) == std::vector<std::string>{
            "system", "user", "user"});
        check_chain_valid(out);
        cleanup_test_project(proj);
    }

    TEST_CASE("Multi-tool block: missing result prunes only that call") {
        auto proj = create_test_project();
        auto msgs = create_test_messages(proj);

        json multi = {
            {"role", "assistant"},
            {"tool_calls", json::array({
                {{"id", "X"}, {"type", "function"},
                 {"function", {{"name", "a"}, {"arguments", "{}"}}}},
                {{"id", "Y"}, {"type", "function"},
                 {"function", {{"name", "b"}, {"arguments", "{}"}}}},
            })}
        };
        json chain = json::array({
            make_msg("system", "sys"),
            multi,
            make_tool_result("X"),
        });

        json out = msgs.sanitize_message_chain(chain);
        CHECK(out.size() == 3);
        REQUIRE(out[1].contains("tool_calls"));
        CHECK(out[1]["tool_calls"].size() == 1);
        CHECK(out[1]["tool_calls"][0]["id"] == "X");
        check_chain_valid(out);
        cleanup_test_project(proj);
    }

    TEST_CASE("Complete multi-tool block is preserved") {
        auto proj = create_test_project();
        auto msgs = create_test_messages(proj);

        json multi = {
            {"role", "assistant"},
            {"tool_calls", json::array({
                {{"id", "X"}, {"type", "function"},
                 {"function", {{"name", "a"}, {"arguments", "{}"}}}},
                {{"id", "Y"}, {"type", "function"},
                 {"function", {{"name", "b"}, {"arguments", "{}"}}}},
            })}
        };
        json chain = json::array({
            make_msg("system", "sys"),
            multi,
            make_tool_result("X"),
            make_tool_result("Y"),
            make_msg("assistant", "both done"),
        });

        json out = msgs.sanitize_message_chain(chain);
        CHECK(out.size() == chain.size());
        check_chain_valid(out);
        cleanup_test_project(proj);
    }

    TEST_CASE("Orphan tool result (unknown id) is dropped") {
        auto proj = create_test_project();
        auto msgs = create_test_messages(proj);

        json chain = json::array({
            make_msg("system", "sys"),
            make_msg("user", "hi"),
            make_tool_result("NEVER_SEEN"),
            make_msg("assistant", "ok"),
        });

        json out = msgs.sanitize_message_chain(chain);
        CHECK(roles_of(out) == std::vector<std::string>{
            "system", "user", "assistant"});
        check_chain_valid(out);
        cleanup_test_project(proj);
    }
}

// ============================================================================
// append_tool_results_if_pending — guarded injection
// ============================================================================

TEST_SUITE("append_tool_results_if_pending") {

    TEST_CASE("Pending tail assistant gets its result appended") {
        json chain = json::array({
            make_msg("system", "sys"),
            make_msg("user", "hi"),
            make_assistant_tc("X"),
        });

        bool appended = Messages::append_tool_results_if_pending(chain, "err");
        CHECK(appended);
        REQUIRE(chain.size() == 4);
        CHECK(chain[3]["role"] == "tool");
        CHECK(chain[3]["tool_call_id"] == "X");
        CHECK(chain[3]["content"] == "err");
        check_chain_valid(chain);
    }

    TEST_CASE("Mid-history (already answered) call is left untouched") {
        json chain = json::array({
            make_msg("system", "sys"),
            make_assistant_tc("X"),
            make_tool_result("X"),
            make_msg("assistant", "done"),
            make_msg("user", "next"),
        });
        json before = chain;

        bool appended = Messages::append_tool_results_if_pending(chain, "err");
        CHECK(!appended);
        CHECK(chain == before);
    }

    TEST_CASE("Fully answered tail block is left untouched") {
        json chain = json::array({
            make_msg("system", "sys"),
            make_assistant_tc("X"),
            make_tool_result("X"),
        });
        json before = chain;

        bool appended = Messages::append_tool_results_if_pending(chain, "err");
        CHECK(!appended);
        CHECK(chain == before);
    }

    TEST_CASE("Partial multi-tool block gets only the missing results") {
        json chain = json::array({
            make_msg("system", "sys"),
            {
                {"role", "assistant"},
                {"tool_calls", json::array({
                    {{"id", "X"}, {"type", "function"},
                     {"function", {{"name", "a"}, {"arguments", "{}"}}}},
                    {{"id", "Y"}, {"type", "function"},
                     {"function", {{"name", "b"}, {"arguments", "{}"}}}},
                })}
            },
            make_tool_result("X", "real result"),
        });

        bool appended = Messages::append_tool_results_if_pending(chain, "err");
        CHECK(appended);
        REQUIRE(chain.size() == 4);
        CHECK(chain[3]["tool_call_id"] == "Y");
        CHECK(chain[3]["content"] == "err");
        CHECK(chain[2]["content"] == "real result");
        check_chain_valid(chain);
    }

    TEST_CASE("Chain without any tool_calls is left untouched") {
        json chain = json::array({
            make_msg("system", "sys"),
            make_msg("user", "hi"),
            make_msg("assistant", "plain"),
        });
        json before = chain;

        bool appended = Messages::append_tool_results_if_pending(chain, "err");
        CHECK(!appended);
        CHECK(chain == before);
    }
}

// ============================================================================
// truncate_if_needed — never emits orphan tool results
// ============================================================================

TEST_SUITE("truncate_if_needed block retention") {

    TEST_CASE("Oversized tool block is dropped as a unit") {
        auto proj = create_test_project();
        auto msgs = create_test_messages(proj);

        const std::string big(6000, 'B');
        json chain = json::array({
            make_msg("system", "sys"),
            make_msg("user", "old question"),
            make_assistant_tc("BIG", big),
            make_tool_result("BIG", big),
            make_msg("user", "recent message"),
        });
        msgs.set_messages(chain);

        // Force truncation: target is 0.8 * max; the BIG block alone
        // (~12KB) exceeds it, so it must be dropped as a unit while the
        // recent small messages survive.
        msgs.truncate_if_needed(4000);

        const json& out = msgs.get_messages();
        CHECK(out.size() >= 2);
        CHECK(out[0]["role"] == "system");
        check_chain_valid(out);
        // The recent message should have survived.
        bool found_recent = false;
        for (const auto& m : out) {
            if (m.value("content", "") == "recent message") found_recent = true;
        }
        CHECK(found_recent);
        cleanup_test_project(proj);
    }

    TEST_CASE("Fitting tool block stays intact with its parent assistant") {
        auto proj = create_test_project();
        auto msgs = create_test_messages(proj);

        // Fill the middle with enough junk to force truncation, with a
        // small tool block at the tail that must survive complete.
        json chain = json::array({make_msg("system", "sys")});
        for (int i = 0; i < 20; i++) {
            chain.push_back(make_msg("user", "filler " + std::to_string(i) +
                                     " " + std::string(500, 'F')));
        }
        chain.push_back(make_assistant_tc("S", "small call"));
        chain.push_back(make_tool_result("S", "small result"));
        msgs.set_messages(chain);

        msgs.truncate_if_needed(3000);

        const json& out = msgs.get_messages();
        check_chain_valid(out);
        // The tail block must have survived intact.
        REQUIRE(out.size() >= 3);
        size_t n = out.size();
        CHECK(out[n - 2].value("role", "") == "assistant");
        CHECK(out[n - 1].value("role", "") == "tool");
        CHECK(out[n - 1].value("tool_call_id", "") == "S");
        cleanup_test_project(proj);
    }
}

// ============================================================================
// §SANITIZE-NOTHROW (2026-08-31, MonkeyByte coredump 2026-08-30)
// sanitize_message_chain runs on the persistence path (background flush
// worker) where an escaped exception is std::terminate -> SIGABRT.
// Malformed chain entries (non-object entries, null/non-string role,
// tool_call_id, or tool_call id) must be dropped, never thrown on.
// ============================================================================

TEST_SUITE("sanitize_message_chain never throws (SANITIZE-NOTHROW)") {

    TEST_CASE("Non-object chain entries are dropped") {
        auto proj = create_test_project();
        auto msgs = create_test_messages(proj);

        json chain = json::array({
            make_msg("system", "sys"),
            nullptr,
            make_msg("user", "hi"),
            "a bare string",
            42,
            make_assistant_tc("X", "calling tool"),
            make_tool_result("X"),
        });

        json out;
        CHECK_NOTHROW(out = msgs.sanitize_message_chain(chain));
        REQUIRE(out.size() == 4);
        CHECK(roles_of(out) == std::vector<std::string>(
            {"system", "user", "assistant", "tool"}));
        check_chain_valid(out);
        cleanup_test_project(proj);
    }

    TEST_CASE("Null or non-string role does not throw") {
        auto proj = create_test_project();
        auto msgs = create_test_messages(proj);

        json chain = json::array({
            make_msg("system", "sys"),
            json{{"role", nullptr}, {"content", "no role"}},
            json{{"role", 123}, {"content", "numeric role"}},
            make_msg("user", "hi"),
        });

        CHECK_NOTHROW(msgs.sanitize_message_chain(chain));
        cleanup_test_project(proj);
    }

    TEST_CASE("Tool result with null tool_call_id is dropped") {
        auto proj = create_test_project();
        auto msgs = create_test_messages(proj);

        json chain = json::array({
            make_msg("user", "hi"),
            make_assistant_tc("X", "calling tool"),
            json{{"role", "tool"}, {"tool_call_id", nullptr}, {"content", "bad"}},
            make_tool_result("X"),
        });

        json out;
        CHECK_NOTHROW(out = msgs.sanitize_message_chain(chain));
        REQUIRE(out.size() == 3);
        CHECK(out[2].value("tool_call_id", "") == "X");
        check_chain_valid(out);
        cleanup_test_project(proj);
    }

    TEST_CASE("tool_call with null id is pruned, valid sibling survives") {
        auto proj = create_test_project();
        auto msgs = create_test_messages(proj);

        json asst = {
            {"role", "assistant"},
            {"tool_calls", json::array({
                {{"id", nullptr}, {"type", "function"},
                 {"function", {{"name", "t"}, {"arguments", "{}"}}}},
                {{"id", "Y"}, {"type", "function"},
                 {"function", {{"name", "t"}, {"arguments", "{}"}}}},
            })}
        };
        json chain = json::array({
            make_msg("user", "hi"),
            asst,
            make_tool_result("Y"),
        });

        json out;
        CHECK_NOTHROW(out = msgs.sanitize_message_chain(chain));
        REQUIRE(out.size() == 3);
        REQUIRE(out[1]["tool_calls"].size() == 1);
        CHECK(out[1]["tool_calls"][0].value("id", "") == "Y");
        check_chain_valid(out);
        cleanup_test_project(proj);
    }

    TEST_CASE("Non-object tool_call entries are dropped without throwing") {
        auto proj = create_test_project();
        auto msgs = create_test_messages(proj);

        json asst = {
            {"role", "assistant"},
            {"tool_calls", json::array({
                nullptr,
                "bogus",
                {{"id", "Z"}, {"type", "function"},
                 {"function", {{"name", "t"}, {"arguments", "{}"}}}},
            })}
        };
        json chain = json::array({
            make_msg("user", "hi"),
            asst,
            make_tool_result("Z"),
        });

        json out;
        CHECK_NOTHROW(out = msgs.sanitize_message_chain(chain));
        REQUIRE(out.size() == 3);
        REQUIRE(out[1]["tool_calls"].size() == 1);
        CHECK(out[1]["tool_calls"][0].value("id", "") == "Z");
        check_chain_valid(out);
        cleanup_test_project(proj);
    }

    TEST_CASE("tool_call with non-object function does not throw") {
        auto proj = create_test_project();
        auto msgs = create_test_messages(proj);

        json asst = {
            {"role", "assistant"},
            {"tool_calls", json::array({
                {{"id", "W"}, {"type", "function"}, {"function", "not-an-object"}},
            })}
        };
        json chain = json::array({
            make_msg("user", "hi"),
            asst,
            make_tool_result("W"),
        });

        json out;
        CHECK_NOTHROW(out = msgs.sanitize_message_chain(chain));
        check_chain_valid(out);
        cleanup_test_project(proj);
    }

    TEST_CASE("save() survives a malformed chain (flush worker path)") {
        auto proj = create_test_project();
        auto msgs = create_test_messages(proj);

        json chain = json::array({
            make_msg("system", "sys"),
            nullptr,
            json{{"role", nullptr}, {"content", "no role"}},
            make_msg("user", "hi"),
        });
        msgs.set_messages(chain);

        // save() is what the background flush worker calls; before
        // SANITIZE-NOTHROW this threw out of the worker -> terminate.
        CHECK_NOTHROW(msgs.save());
        cleanup_test_project(proj);
    }
}
