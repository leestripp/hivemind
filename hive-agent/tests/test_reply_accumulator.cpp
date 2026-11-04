#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <string>

#include "core/ReplyAccumulator.h"

using json = nlohmann::json;

namespace {

json tool_result(std::string_view content_key, const std::string& value) {
    return json{{std::string(content_key), value}};
}

}  // namespace

TEST_SUITE("ReplyAccumulator") {
    TEST_CASE("plain final assistant prose is the reply") {
        ReplyAccumulator replies;
        replies.begin();
        replies.append_assistant("All done.");
        CHECK(replies.user_output() == "All done.");
    }

    TEST_CASE("intermediate tool prose is not concatenated with final prose") {
        ReplyAccumulator replies;
        replies.begin();
        replies.append_assistant("I will inspect it.");
        replies.on_tool_call("execute_bash", json::object());
        replies.append_assistant("The fix is verified.");
        CHECK(replies.user_output() == "The fix is verified.");
    }

    TEST_CASE("stop summary and intermediate prose are ordered fallbacks") {
        ReplyAccumulator with_summary;
        with_summary.begin();
        with_summary.append_assistant("Working context.");
        with_summary.on_tool_call("execute_bash", json::object());
        with_summary.on_tool_result("stop", tool_result("summary", "Rescue summary."));
        CHECK(with_summary.user_output() == "Rescue summary.");

        ReplyAccumulator without_summary;
        without_summary.begin();
        without_summary.append_assistant("Working context.");
        without_summary.on_tool_call("execute_bash", json::object());
        CHECK(without_summary.user_output() == "Working context.");
    }

    TEST_CASE("stop summary is not appended to an existing reply") {
        ReplyAccumulator replies;
        replies.begin();
        replies.append_assistant("Final response.");
        replies.on_tool_call("stop", json::object());
        replies.on_tool_result("stop", tool_result("summary", "Repeated summary."));
        CHECK(replies.user_output() == "Final response.");
    }

    TEST_CASE("prose before normalized Done stays the final reply") {
        for (const char* state : {"Done", "done", "DONE", " DoNe ", "\tDone\n"}) {
            CAPTURE(state);
            ReplyAccumulator replies;
            replies.begin();
            replies.on_tool_result("tell_user", tool_result("content", "Committed."));
            replies.append_assistant("Final response.");
            replies.on_tool_call("set_status", json{{"state", state}});
            CHECK(replies.user_output() == "Committed.\n\nFinal response.");
        }
    }

    TEST_CASE("nonterminal status still marks preceding prose intermediate") {
        ReplyAccumulator replies;
        replies.begin();
        replies.on_tool_result("tell_user", tool_result("content", "Committed."));
        replies.append_assistant("Still working.");
        replies.on_tool_call("set_status", json{{"state", " Acting "}});
        CHECK(replies.user_output() == "Committed.");

        replies.begin();
        replies.append_assistant("Fallback working text.");
        replies.on_tool_call("set_status", json{{"state", "Verifying"}});
        CHECK(replies.user_output() == "Fallback working text.");
    }

    TEST_CASE("begin resets every output channel") {
        ReplyAccumulator replies;
        replies.append_assistant("Old final.");
        replies.on_tool_result("tell_user", tool_result("content", "Old committed."));
        replies.on_tool_result("stop", tool_result("summary", "Old summary."));
        replies.begin();
        CHECK(replies.user_output().empty());
    }
}
