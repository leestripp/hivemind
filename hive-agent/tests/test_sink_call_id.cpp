// SPDX-License-Identifier: BSL-1.0
// tests/test_sink_call_id.cpp
//
// §Phase D1: verifies that the 3-arg AgentSink overloads propagate
// the OpenAI-style call_id correctly. The 2-arg versions (back-compat)
// must still work — sinks that don't care about correlation keep
// overriding only those.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "library/AgentSink.h"

#include <string>
#include <vector>

namespace {

// Sink that overrides the 3-arg versions and captures call_ids.
struct IdRecordingSink : public hive::AgentSink {
    struct Event {
        std::string kind;        // "call" | "progress" | "result"
        std::string call_id;
        std::string name;
    };
    std::vector<Event> events;

    void on_tool_call(std::string_view name,
                      std::string_view call_id,
                      const nlohmann::json& args) noexcept override {
        events.push_back({"call", std::string(call_id), std::string(name)});
        (void)args;
    }
    void on_tool_progress(std::string_view name,
                          std::string_view call_id,
                          const nlohmann::json& progress) noexcept override {
        events.push_back({"progress", std::string(call_id), std::string(name)});
        (void)progress;
    }
    void on_tool_result(std::string_view name,
                        std::string_view call_id,
                        const nlohmann::json& result) noexcept override {
        events.push_back({"result", std::string(call_id), std::string(name)});
        (void)result;
    }
};

// Sink that overrides only the 2-arg versions (back-compat).
struct LegacySink : public hive::AgentSink {
    int call_count   = 0;
    int result_count = 0;
    int progress_count = 0;

    void on_tool_call(std::string_view,
                      const nlohmann::json&) noexcept override {
        ++call_count;
    }
    void on_tool_result(std::string_view,
                        const nlohmann::json&) noexcept override {
        ++result_count;
    }
    void on_tool_progress(std::string_view,
                          const nlohmann::json&) noexcept override {
        ++progress_count;
    }
};

}  // namespace

TEST_CASE("D1.1: 3-arg sink receives call_id for all 3 events") {
    IdRecordingSink sink;
    nlohmann::json args   = {{"command", "ls"}};
    nlohmann::json prog   = {{"stage", "executing"}, {"percent", 50}};
    nlohmann::json result = {{"status", "success"}};

    sink.on_tool_call("execute_bash", "call_abc123", args);
    sink.on_tool_progress("execute_bash", "call_abc123", prog);
    sink.on_tool_result("execute_bash", "call_abc123", result);

    REQUIRE(sink.events.size() == 3);
    CHECK(sink.events[0].kind     == "call");
    CHECK(sink.events[0].call_id  == "call_abc123");
    CHECK(sink.events[0].name     == "execute_bash");
    CHECK(sink.events[1].kind     == "progress");
    CHECK(sink.events[1].call_id  == "call_abc123");
    CHECK(sink.events[2].kind     == "result");
    CHECK(sink.events[2].call_id  == "call_abc123");
}

TEST_CASE("D1.2: 3-arg sink with empty call_id still receives the event") {
    IdRecordingSink sink;
    sink.on_tool_call("read_file", "", nlohmann::json::object());
    REQUIRE(sink.events.size() == 1);
    CHECK(sink.events[0].call_id == "");
}

TEST_CASE("D1.3: 2-arg-only sink still fires (3-arg default delegates)") {
    // The 3-arg default in AgentSink calls the 2-arg version. Sinks
    // that override only 2-arg (back-compat path) must still receive
    // every event when LLMClient dispatches through the 3-arg version.
    //
    // Note: we call through a base pointer — that's how LLMClient
    // does it. Calling `legacy_sink.on_tool_call(...)` directly would
    // hit C++ name hiding (the 2-arg override hides the 3-arg base
    // overload in derived-class scope), which is exactly the scenario
    // we DON'T want to test.
    LegacySink legacy;
    hive::AgentSink* sink = &legacy;
    sink->on_tool_call("execute_bash", "call_xyz",
                       nlohmann::json{{"command", "ls"}});
    sink->on_tool_progress("execute_bash", "call_xyz",
                          nlohmann::json{{"stage", "executing"}});
    sink->on_tool_result("execute_bash", "call_xyz",
                        nlohmann::json{{"status", "success"}});

    CHECK(legacy.call_count     == 1);
    CHECK(legacy.progress_count == 1);
    CHECK(legacy.result_count   == 1);
}

TEST_CASE("D1.4: default no-op sink swallows everything") {
    // The default sink (no overrides) must compile and link.
    hive::AgentSink sink;
    sink.on_tool_call("x", "id", nlohmann::json::object());
    sink.on_tool_progress("x", "id", nlohmann::json::object());
    sink.on_tool_result("x", "id", nlohmann::json::object());
    // No crash = pass.
    CHECK(true);
}

TEST_CASE("D1.5: same call_id across the 3 events allows correlation") {
    IdRecordingSink sink;
    const std::string cid = "call_unique_42";

    sink.on_tool_call    ("execute_bash", cid, nlohmann::json::object());
    sink.on_tool_progress("execute_bash", cid, nlohmann::json::object());
    sink.on_tool_result  ("execute_bash", cid, nlohmann::json::object());

    // All three share the same id — that's what lets the webui
    // group them into one card.
    REQUIRE(sink.events.size() == 3);
    CHECK(sink.events[0].call_id == sink.events[1].call_id);
    CHECK(sink.events[1].call_id == sink.events[2].call_id);
}