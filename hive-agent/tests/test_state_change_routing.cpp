/**
 * @file test_state_change_routing.cpp
 * @brief §State Machine — tests for LLMClient::handle_set_state_result().
 *
 * Verifies that LLM-driven state transitions (via the `set_status` tool)
 * are dispatched correctly:
 *   1. Valid set_state payloads invoke the wired StateChangeCallback with
 *      the parsed public AgentState (the normal Agent path — AgentImpl is
 *      the single source of truth for the public state).
 *   2. Malformed / unknown-state payloads return false and dispatch nothing.
 *   3. Without a callback, the fallback fires AgentSink::on_state_change
 *      directly with locally tracked prev (direct LLMClient users).
 *   4. With neither callback nor sink, nothing crashes.
 */

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <filesystem>
#include <string>
#include <utility>
#include <vector>

#include "api/LLMClient.h"
#include "types/AgentState.h"

using json = nlohmann::json;

namespace {

// Minimal recording sink for the fallback path.
struct RecordingSink : hive::AgentSink {
    std::vector<std::pair<int, int>> transitions;
    void on_state_change(int from, int to) noexcept override {
        transitions.emplace_back(from, to);
    }
};

AppConfig make_test_config(const std::filesystem::path& dir) {
    AppConfig cfg;
    cfg.target_directory = dir;
    cfg.memory_disabled = true;   // hermetic: no SessionMemory on disk
    return cfg;
}

std::filesystem::path make_temp_dir(const std::string& tag) {
    auto dir = std::filesystem::temp_directory_path()
             / ("daedalus_state_routing_" + tag);
    std::filesystem::create_directories(dir);
    return dir;
}

}  // namespace

TEST_SUITE("handle_set_state_result — callback path (normal Agent wiring)") {

    TEST_CASE("Valid set_state payload invokes callback with parsed state") {
        LLMClient client(make_test_config(make_temp_dir("cb_valid")));
        std::vector<hive::AgentState> seen;
        client.set_state_change_callback(
            [&seen](hive::AgentState s) { seen.push_back(s); });

        json result = {{"status", "set_state"}, {"state", "Acting"}};
        CHECK(client.handle_set_state_result(result) == true);
        REQUIRE(seen.size() == 1);
        CHECK(seen[0] == hive::AgentState::Acting);
    }

    TEST_CASE("State name matching is case-insensitive") {
        LLMClient client(make_test_config(make_temp_dir("cb_case")));
        std::vector<hive::AgentState> seen;
        client.set_state_change_callback(
            [&seen](hive::AgentState s) { seen.push_back(s); });

        CHECK(client.handle_set_state_result(
                  {{"status", "set_state"}, {"state", "verifying"}}) == true);
        REQUIRE(seen.size() == 1);
        CHECK(seen[0] == hive::AgentState::Verifying);
    }

    TEST_CASE("A full turn sequence is forwarded in order") {
        LLMClient client(make_test_config(make_temp_dir("cb_seq")));
        std::vector<hive::AgentState> seen;
        client.set_state_change_callback(
            [&seen](hive::AgentState s) { seen.push_back(s); });

        for (const char* name : {"Planning", "Acting", "Verifying", "Done"}) {
            CHECK(client.handle_set_state_result(
                      {{"status", "set_state"}, {"state", name}}) == true);
        }
        REQUIRE(seen.size() == 4);
        CHECK(seen[0] == hive::AgentState::Planning);
        CHECK(seen[1] == hive::AgentState::Acting);
        CHECK(seen[2] == hive::AgentState::Verifying);
        CHECK(seen[3] == hive::AgentState::Done);
    }
}

TEST_SUITE("handle_set_state_result — rejected payloads") {

    TEST_CASE("Non-set_state results return false and dispatch nothing") {
        LLMClient client(make_test_config(make_temp_dir("rej_other")));
        int calls = 0;
        client.set_state_change_callback(
            [&calls](hive::AgentState) { ++calls; });

        CHECK(client.handle_set_state_result(
                  {{"status", "success"}}) == false);
        CHECK(client.handle_set_state_result(
                  {{"status", "error"}, {"message", "boom"}}) == false);
        CHECK(calls == 0);
    }

    TEST_CASE("Missing or wrong-type 'state' returns false") {
        LLMClient client(make_test_config(make_temp_dir("rej_shape")));
        int calls = 0;
        client.set_state_change_callback(
            [&calls](hive::AgentState) { ++calls; });

        CHECK(client.handle_set_state_result(
                  {{"status", "set_state"}}) == false);
        CHECK(client.handle_set_state_result(
                  {{"status", "set_state"}, {"state", 42}}) == false);
        CHECK(calls == 0);
    }

    TEST_CASE("Unknown state name returns false") {
        LLMClient client(make_test_config(make_temp_dir("rej_bogus")));
        int calls = 0;
        client.set_state_change_callback(
            [&calls](hive::AgentState) { ++calls; });

        CHECK(client.handle_set_state_result(
                  {{"status", "set_state"}, {"state", "Bogus"}}) == false);
        CHECK(calls == 0);
    }

    TEST_CASE("Non-object results return false") {
        LLMClient client(make_test_config(make_temp_dir("rej_nonobj")));
        int calls = 0;
        client.set_state_change_callback(
            [&calls](hive::AgentState) { ++calls; });

        CHECK(client.handle_set_state_result(json("set_state")) == false);
        CHECK(client.handle_set_state_result(json(42)) == false);
        CHECK(calls == 0);
    }
}

TEST_SUITE("handle_set_state_result — sink fallback (no callback wired)") {

    TEST_CASE("Fires on_state_change directly with tracked prev") {
        LLMClient client(make_test_config(make_temp_dir("fb_basic")));
        auto sink = std::make_shared<RecordingSink>();
        client.set_sink(sink);

        CHECK(client.handle_set_state_result(
                  {{"status", "set_state"}, {"state", "Acting"}}) == true);
        CHECK(client.handle_set_state_result(
                  {{"status", "set_state"}, {"state", "Verifying"}}) == true);

        const int acting = static_cast<int>(hive::AgentState::Acting);
        const int verifying = static_cast<int>(hive::AgentState::Verifying);
        REQUIRE(sink->transitions.size() == 2);
        // First transition: prev starts at Idle (0).
        CHECK(sink->transitions[0].first  == static_cast<int>(hive::AgentState::Idle));
        CHECK(sink->transitions[0].second == acting);
        // Second transition: prev is the last dispatched state.
        CHECK(sink->transitions[1].first  == acting);
        CHECK(sink->transitions[1].second == verifying);
    }

    TEST_CASE("No callback and no sink — still returns true, no crash") {
        LLMClient client(make_test_config(make_temp_dir("fb_nosink")));
        CHECK(client.handle_set_state_result(
                  {{"status", "set_state"}, {"state", "Done"}}) == true);
    }
}
