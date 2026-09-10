/**
 * @file test_set_status_tool.cpp
 * @brief §State Machine — tests for SetStatusTool.
 *
 * These tests verify:
 *   1. Tool definition (JSON schema) is well-formed.
 *   2. Valid state names are accepted (case-insensitive) and normalised.
 *   3. Invalid state names return a structured error.
 *   4. Missing / wrong-type 'state' argument returns a structured error.
 *   5. The success status string is "set_state" so LLMClient can
 *      branch on it and fire the public state-change event.
 *   6. agent_state_from() (the inverse of to_string()) round-trips.
 *   7. §IdleLoopFix (2026-08-20): runtime-owned states (Idle,
 *      AwaitingApproval, Compressing) are REJECTED with an actionable
 *      error.  A model used to call set_status("Idle") expecting the
 *      agent loop to exit ("Idle = no work in flight"), got
 *      status=success, and looped forever.  The loop ends only via the
 *      `stop` tool — the error message for Idle says exactly that.
 */

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <string>
#include <filesystem>

#include "core/FileIO.h"
#include "tools/SetStatusTool.h"
#include "types/AgentState.h"

using json = nlohmann::json;

// ============================================================================
// Suite 1: Tool definition is well-formed.
// ============================================================================

TEST_SUITE("SetStatusTool — JSON schema") {

    TEST_CASE("Tool name is 'set_status'") {
        SetStatusTool tool;
        CHECK(tool.get_name() == "set_status");
    }

    TEST_CASE("Definition declares 'state' as a required string") {
        SetStatusTool tool;
        json def = tool.get_definition();
        REQUIRE(def.contains("function"));
        const auto& fn = def["function"];
        REQUIRE(fn.contains("parameters"));
        const auto& params = fn["parameters"];
        REQUIRE(params.contains("properties"));
        REQUIRE(params["properties"].contains("state"));
        CHECK(params["properties"]["state"]["type"] == "string");
        REQUIRE(params.contains("required"));
        CHECK(params["required"].is_array());
        CHECK(params["required"].size() == 1);
        CHECK(params["required"][0] == "state");
    }
}

// ============================================================================
// Suite 2: Valid state names — accepted, normalised.
// ============================================================================

TEST_SUITE("SetStatusTool — valid input") {

    TEST_CASE("Every model-driven AgentState name round-trips") {
        SetStatusTool tool;
        FileIO io(std::filesystem::temp_directory_path());  // unused by SetStatusTool
        const std::vector<std::string> valid = {
            "Understanding", "Planning", "Acting", "Verifying",
            "Documenting", "Done", "Cancelled", "Error",
        };
        for (const auto& name : valid) {
            json args = {{"state", name}};
            json r = tool.execute(args, io);
            CHECK(r["status"] == "set_state");
            CHECK(r["state"]  == name);
        }
    }

    TEST_CASE("Lower-case input is normalised to title-case") {
        SetStatusTool tool;
        FileIO io(std::filesystem::temp_directory_path());
        json args = {{"state", "planning"}};
        json r = tool.execute(args, io);
        CHECK(r["status"] == "set_state");
        CHECK(r["state"]  == "Planning");
    }

    TEST_CASE("UPPER-CASE input is normalised") {
        SetStatusTool tool;
        FileIO io(std::filesystem::temp_directory_path());
        json args = {{"state", "ACTING"}};
        json r = tool.execute(args, io);
        CHECK(r["status"] == "set_state");
        CHECK(r["state"]  == "Acting");
    }

    TEST_CASE("MiXeD case is normalised") {
        SetStatusTool tool;
        FileIO io(std::filesystem::temp_directory_path());
        json args = {{"state", "VeRiFyInG"}};
        json r = tool.execute(args, io);
        CHECK(r["status"] == "set_state");
        CHECK(r["state"]  == "Verifying");
    }

    TEST_CASE("Leading/trailing whitespace is trimmed") {
        SetStatusTool tool;
        FileIO io(std::filesystem::temp_directory_path());
        json args = {{"state", "  Done \t"}};
        json r = tool.execute(args, io);
        CHECK(r["status"] == "set_state");
        CHECK(r["state"]  == "Done");
    }
}

// ============================================================================
// Suite 2b: §IdleLoopFix — runtime-owned states are rejected.
// ============================================================================

TEST_SUITE("SetStatusTool — runtime-owned states rejected") {

    TEST_CASE("Idle returns an error that points at the stop tool") {
        SetStatusTool tool;
        FileIO io(std::filesystem::temp_directory_path());
        json r = tool.execute({{"state", "Idle"}}, io);
        CHECK(r["status"] == "error");
        REQUIRE(r.contains("message"));
        std::string msg = r["message"].get<std::string>();
        // The model must learn: runtime-owned + set_status never exits
        // the loop + stop is the exit hatch.
        CHECK(msg.find("runtime-owned") != std::string::npos);
        CHECK(msg.find("does NOT exit the agent loop") != std::string::npos);
        CHECK(msg.find("stop") != std::string::npos);
        // …and it must list the states it CAN use so it can self-correct.
        CHECK(msg.find("Planning") != std::string::npos);
        CHECK(msg.find("Done")     != std::string::npos);
    }

    TEST_CASE("Idle is rejected case-insensitively and with whitespace") {
        SetStatusTool tool;
        FileIO io(std::filesystem::temp_directory_path());
        for (const auto& variant : {"idle", "IDLE", "  Idle \t", "iDlE"}) {
            json r = tool.execute({{"state", variant}}, io);
            CHECK(r["status"] == "error");
            CHECK(r["message"].get<std::string>().find("runtime-owned")
                  != std::string::npos);
        }
    }

    TEST_CASE("AwaitingApproval and Compressing are rejected as runtime-owned") {
        SetStatusTool tool;
        FileIO io(std::filesystem::temp_directory_path());
        for (const auto& name : {"AwaitingApproval", "Compressing"}) {
            json r = tool.execute({{"state", name}}, io);
            CHECK(r["status"] == "error");
            std::string msg = r["message"].get<std::string>();
            CHECK(msg.find(name) != std::string::npos);
            CHECK(msg.find("runtime-owned") != std::string::npos);
        }
    }

    TEST_CASE("JSON-string form of Idle is also rejected") {
        SetStatusTool tool;
        FileIO io(std::filesystem::temp_directory_path());
        json args = std::string(R"({"state":"Idle"})");
        json r = tool.execute(args, io);
        CHECK(r["status"] == "error");
        CHECK(r["message"].get<std::string>().find("stop") != std::string::npos);
    }

    TEST_CASE("Error result never carries status==set_state (no state event fired)") {
        // LLMClient::handle_set_state_result() branches on
        // status == "set_state"; an error payload must therefore never
        // reach the sink.  Belt-and-braces check on the contract.
        SetStatusTool tool;
        FileIO io(std::filesystem::temp_directory_path());
        for (const auto& name : {"Idle", "AwaitingApproval", "Compressing"}) {
            json r = tool.execute({{"state", name}}, io);
            CHECK(r["status"] != "set_state");
            CHECK_FALSE(r.contains("state"));
        }
    }
}

// ============================================================================
// Suite 3: Invalid input — structured error.
// ============================================================================

TEST_SUITE("SetStatusTool — invalid input") {

    TEST_CASE("Unknown state returns error listing valid names") {
        SetStatusTool tool;
        FileIO io(std::filesystem::temp_directory_path());
        json args = {{"state", "Foo"}};
        json r = tool.execute(args, io);
        CHECK(r["status"]  == "error");
        CHECK(r.contains("message"));
        std::string msg = r["message"].get<std::string>();
        CHECK(msg.find("Foo") != std::string::npos);
        // Message should list every valid state so the model can self-correct.
        CHECK(msg.find("Planning")    != std::string::npos);
        CHECK(msg.find("Acting")      != std::string::npos);
        CHECK(msg.find("Verifying")   != std::string::npos);
        CHECK(msg.find("Documenting") != std::string::npos);
        CHECK(msg.find("Done")        != std::string::npos);
    }

    TEST_CASE("Empty string returns error") {
        SetStatusTool tool;
        FileIO io(std::filesystem::temp_directory_path());
        json args = {{"state", ""}};
        json r = tool.execute(args, io);
        CHECK(r["status"] == "error");
    }

    TEST_CASE("Whitespace-only string returns error") {
        SetStatusTool tool;
        FileIO io(std::filesystem::temp_directory_path());
        json args = {{"state", "   \t  "}};
        json r = tool.execute(args, io);
        CHECK(r["status"] == "error");
    }

    TEST_CASE("Missing 'state' key returns error") {
        SetStatusTool tool;
        FileIO io(std::filesystem::temp_directory_path());
        json args = json::object();
        json r = tool.execute(args, io);
        CHECK(r["status"] == "error");
    }

    TEST_CASE("Non-string 'state' returns error") {
        SetStatusTool tool;
        FileIO io(std::filesystem::temp_directory_path());
        json args = {{"state", 42}};
        json r = tool.execute(args, io);
        CHECK(r["status"] == "error");
    }

    TEST_CASE("JSON-string form (legacy clients) is also accepted") {
        SetStatusTool tool;
        FileIO io(std::filesystem::temp_directory_path());
        // Some legacy LLM clients send the args as a JSON-encoded string
        // rather than an object.  SetStatusTool accepts both.
        json args = std::string(R"({"state":"Done"})");
        json r = tool.execute(args, io);
        CHECK(r["status"] == "set_state");
        CHECK(r["state"]  == "Done");
    }

    TEST_CASE("JSON-string form with unknown state still errors") {
        SetStatusTool tool;
        FileIO io(std::filesystem::temp_directory_path());
        json args = std::string(R"({"state":"Bogus"})");
        json r = tool.execute(args, io);
        CHECK(r["status"] == "error");
    }
}

// ============================================================================
// Suite 4: agent_state_from() — the public inverse of to_string().
// ============================================================================

TEST_SUITE("hive::agent_state_from") {

    TEST_CASE("Round-trips every public state") {
        bool ok = false;
        CHECK(hive::agent_state_from("Idle", &ok)             == hive::AgentState::Idle);             CHECK(ok);
        CHECK(hive::agent_state_from("Understanding", &ok)    == hive::AgentState::Understanding);    CHECK(ok);
        CHECK(hive::agent_state_from("Planning", &ok)         == hive::AgentState::Planning);         CHECK(ok);
        CHECK(hive::agent_state_from("Acting", &ok)           == hive::AgentState::Acting);           CHECK(ok);
        CHECK(hive::agent_state_from("Verifying", &ok)        == hive::AgentState::Verifying);        CHECK(ok);
        CHECK(hive::agent_state_from("Documenting", &ok)      == hive::AgentState::Documenting);      CHECK(ok);
        CHECK(hive::agent_state_from("AwaitingApproval", &ok) == hive::AgentState::AwaitingApproval); CHECK(ok);
        CHECK(hive::agent_state_from("Compressing", &ok)      == hive::AgentState::Compressing);      CHECK(ok);
        CHECK(hive::agent_state_from("Done", &ok)             == hive::AgentState::Done);             CHECK(ok);
        CHECK(hive::agent_state_from("Cancelled", &ok)        == hive::AgentState::Cancelled);        CHECK(ok);
        CHECK(hive::agent_state_from("Error", &ok)            == hive::AgentState::Error);            CHECK(ok);
    }

    TEST_CASE("Case-insensitive matching") {
        bool ok = false;
        CHECK(hive::agent_state_from("planning",   &ok) == hive::AgentState::Planning); CHECK(ok);
        CHECK(hive::agent_state_from("ACTING",     &ok) == hive::AgentState::Acting);   CHECK(ok);
        CHECK(hive::agent_state_from("VeRiFyInG",  &ok) == hive::AgentState::Verifying);CHECK(ok);
    }

    TEST_CASE("Unknown name returns Idle and ok=false") {
        bool ok = true;
        auto s = hive::agent_state_from("Bogus", &ok);
        CHECK(s == hive::AgentState::Idle);
        CHECK_FALSE(ok);
    }

    TEST_CASE("Empty string returns Idle and ok=false") {
        bool ok = true;
        auto s = hive::agent_state_from("", &ok);
        CHECK(s == hive::AgentState::Idle);
        CHECK_FALSE(ok);
    }

    TEST_CASE("ok pointer may be nullptr (callers can ignore the flag)") {
        // Must not crash.
        CHECK(hive::agent_state_from("Done")             == hive::AgentState::Done);
        CHECK(hive::agent_state_from("Bogus")            == hive::AgentState::Idle);
    }
}

// ============================================================================
// Suite 5: Round-trip — to_string(agent_state_from(name)) == Normalised(name).
// ============================================================================

TEST_SUITE("to_string ↔ agent_state_from round-trip") {

    TEST_CASE("Every public name survives a round-trip") {
        const std::vector<std::string> names = {
            "Idle", "Understanding", "Planning", "Acting", "Verifying",
            "Documenting", "AwaitingApproval", "Compressing",
            "Done", "Cancelled", "Error",
        };
        for (const auto& name : names) {
            bool ok = false;
            auto s = hive::agent_state_from(name, &ok);
            REQUIRE(ok);
            CHECK(std::string(hive::to_string(s)) == name);
        }
    }
}
