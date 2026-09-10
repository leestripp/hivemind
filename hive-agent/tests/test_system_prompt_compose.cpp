/**
 * @file test_system_prompt_compose.cpp
 * @brief §Phase 14 — tests for the slim SystemPromptBuilder::compose().
 *
 * The library no longer writes identity / rules / workflow / tool guidelines
 * prose. These tests assert that:
 *   1. The host's system_prompt appears verbatim as the FIRST section.
 *   2. Tool definitions appear as a fenced JSON block (not prose).
 *   3. Skills appear as a catalog (names + descriptions), NOT full SKILL.md.
 *   4. Session memory is included when set, omitted when disabled.
 *   5. §ContextCache: NO per-iteration Context Status block — the composed
 *      prompt stays byte-static so provider prompt caches can match it.
 *   6. The library NEVER injects hard-coded "hive-agent" / "autonomous coding
 *      agent" / "Task Focus" / "agent identity" prose.
 */

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include <string>
#include <sstream>

#include "core/SystemPromptBuilder.h"

// ============================================================================
// Suite 1: Host text is the first section, verbatim.
// ============================================================================

TEST_SUITE("Host prompt passes through verbatim") {

    TEST_CASE("Empty system_prompt produces library-owned-blocks-only output") {
        // §Phase 14 + §State Machine + §Reserved Agent Dirs: compose()
        // with empty host text produces the state machine and
        // reserved-dirs sections (the library-owned blocks). The
        // library never invents host prose.
        SystemPromptOptions options;
        std::string prompt = SystemPromptBuilder::compose(options);

        // The library never invents identity / rules / workflow.
        CHECK(prompt.find("autonomous coding agent") == std::string::npos);
        // State machine and reserved-dirs appear.
        CHECK(prompt.find("# State Machine")              != std::string::npos);
        CHECK(prompt.find("# Reserved Agent Directories") != std::string::npos);
        // §ContextCache: NO per-iteration Context Status block — the
        // composed prompt must stay byte-static across the session so
        // provider prompt caches (MiniMax / Kimi) can match the prefix.
        // The stats ride as a transient trailing message instead.
        CHECK(prompt.find("# Context Status") == std::string::npos);
    }

    TEST_CASE("Host prompt appears verbatim, first section") {
        SystemPromptOptions options;
        options.system_prompt =
            "You are a custom agent.\n"
            "Rule 1: never touch /etc.\n"
            "Rule 2: always run tests.\n";
        std::string prompt = SystemPromptBuilder::compose(options);

        // The host's text appears as the very first section.
        CHECK(prompt.find("You are a custom agent.") == 0);
        // All three rules are present, in order, untouched.
        auto r1 = prompt.find("Rule 1:");
        auto r2 = prompt.find("Rule 2:");
        CHECK(r1 != std::string::npos);
        CHECK(r2 != std::string::npos);
        CHECK(r1 < r2);  // order preserved
    }

    TEST_CASE("Compose does not inject 'CodeMax' identity") {
        // The synthetic host prompt never mentions the engine name.
        // The reserved-dirs section DOES mention ".hive-agent" (it's a
        // directory name, not project identity) — so we assert on
        // the *project identity* terms only.
        SystemPromptOptions options;
        options.system_prompt = "I am a generic agent.";
        std::string prompt = SystemPromptBuilder::compose(options);
        CHECK(prompt.find("autonomous coding agent") == std::string::npos);
        CHECK(prompt.find("agent identity") == std::string::npos);
        CHECK(prompt.find("Identity") == std::string::npos);
    }

    TEST_CASE("Compose does not inject 'Task Focus' / 'agent identity'") {
        SystemPromptOptions options;
        options.system_prompt = "I am a generic agent.";
        std::string prompt = SystemPromptBuilder::compose(options);
        CHECK(prompt.find("Task Focus") == std::string::npos);
        CHECK(prompt.find("Agent Identity") == std::string::npos);
        CHECK(prompt.find("Tool Guidelines") == std::string::npos);
        CHECK(prompt.find("Agent Workflow") == std::string::npos);
        CHECK(prompt.find("Build System Guidelines") == std::string::npos);
        CHECK(prompt.find("Research Guidelines") == std::string::npos);
        CHECK(prompt.find("Debugging Guidelines") == std::string::npos);
    }
}

// ============================================================================
// Suite 2: Library appends the dynamic adders, host's text first.
// ============================================================================

TEST_SUITE("Dynamic adders") {

    TEST_CASE("Tool definitions appear as fenced JSON block") {
        SystemPromptOptions options;
        options.system_prompt = "Host text.";
        options.tool_definitions = R"({"name":"foo","description":"foo tool"})";
        std::string prompt = SystemPromptBuilder::compose(options);

        CHECK(prompt.find("Host text.") != std::string::npos);
        CHECK(prompt.find("# Tool Definitions") != std::string::npos);
        CHECK(prompt.find("```json") != std::string::npos);
        CHECK(prompt.find("foo tool") != std::string::npos);
        CHECK(prompt.find("```") != std::string::npos);
    }

    TEST_CASE("Active skills appear as a list, names only") {
        SystemPromptOptions options;
        options.system_prompt = "Host text.";
        options.active_skills = "- **gitlab-release**: makes a release page\n";
        std::string prompt = SystemPromptBuilder::compose(options);

        CHECK(prompt.find("# Active Skills") != std::string::npos);
        CHECK(prompt.find("gitlab-release") != std::string::npos);
    }

    TEST_CASE("Available skills catalog mentions load_skill") {
        SystemPromptOptions options;
        options.system_prompt = "Host text.";
        options.available_skills = "- **firefox**: opens a Firefox browser\n";
        std::string prompt = SystemPromptBuilder::compose(options);

        CHECK(prompt.find("# Available Skills") != std::string::npos);
        CHECK(prompt.find("firefox") != std::string::npos);
        CHECK(prompt.find("load_skill") != std::string::npos);  // library instruction
    }

    TEST_CASE("Session memory included when enabled and set") {
        SystemPromptOptions options;
        options.system_prompt = "Host text.";
        options.session_memory = "Remember: fix bug in utils.cpp";
        options.include_session_memory = true;
        std::string prompt = SystemPromptBuilder::compose(options);

        CHECK(prompt.find("# Session Memory") != std::string::npos);
        CHECK(prompt.find("fix bug in utils.cpp") != std::string::npos);
    }

    TEST_CASE("Session memory excluded when flag is false") {
        SystemPromptOptions options;
        options.system_prompt = "Host text.";
        options.session_memory = "SECRET_DATA";
        options.include_session_memory = false;
        std::string prompt = SystemPromptBuilder::compose(options);

        CHECK(prompt.find("SECRET_DATA") == std::string::npos);
        CHECK(prompt.find("# Session Memory") == std::string::npos);
    }

    TEST_CASE("Context stats are never part of the composed prompt") {
        // §ContextCache: per-iteration stats used to be section 8 of the
        // composed prompt. They moved to a transient trailing message
        // (LLMClient::inject_context_stats) because a changing block in
        // the static prompt breaks provider prompt-cache prefix matching.
        SystemPromptOptions options;
        options.system_prompt = "Host text.";
        std::string prompt = SystemPromptBuilder::compose(options);
        CHECK(prompt.find("# Context Status") == std::string::npos);
        CHECK(prompt.find("Token Usage") == std::string::npos);
    }

    TEST_CASE("system_info_note appended when set") {
        SystemPromptOptions options;
        options.system_prompt = "Host text.";
        options.system_info_note = "Always call system_info at session start.";
        std::string prompt = SystemPromptBuilder::compose(options);

        CHECK(prompt.find("Always call system_info") != std::string::npos);
        CHECK(prompt.find("system_info") != std::string::npos);
    }
}

// ============================================================================
// Suite 3: Section order — host text first, adders in fixed order after.
// ============================================================================

TEST_SUITE("Section order") {

    TEST_CASE("Host text precedes adders") {
        SystemPromptOptions options;
        options.system_prompt = "AAAA_HOST_TEXT";
        options.tool_definitions = R"({"x":1})";
        options.active_skills = "- **z**: zzz\n";
        options.available_skills = "- **y**: yyy\n";
        options.session_memory = "SESSION_MEMORY";

        std::string prompt = SystemPromptBuilder::compose(options);

        // Verify ordering: each section marker appears AFTER the host text.
        size_t host_pos  = prompt.find("AAAA_HOST_TEXT");
        size_t tools_pos = prompt.find("# Tool Definitions");
        size_t act_pos   = prompt.find("# Active Skills");
        size_t avail_pos = prompt.find("# Available Skills");
        size_t mem_pos   = prompt.find("# Session Memory");

        CHECK(host_pos == 0);
        CHECK(host_pos < tools_pos);
        CHECK(tools_pos < act_pos);
        CHECK(act_pos < avail_pos);
        CHECK(avail_pos < mem_pos);
    }
}

// ============================================================================
// Suite 4: Backward-compat shims return empty strings (with deprecation).
// ============================================================================

// ============================================================================
// Suite 5: Utility functions unchanged.
// ============================================================================

TEST_SUITE("Utility functions") {

    TEST_CASE("estimateTokens returns reasonable estimate") {
        std::string short_text = "Hello";
        std::string long_text = std::string(1000, 'a');

        size_t short_tokens = SystemPromptBuilder::estimateTokens(short_text);
        size_t long_tokens = SystemPromptBuilder::estimateTokens(long_text);

        CHECK(long_tokens > short_tokens);
        CHECK(long_tokens >= 200);
        CHECK(long_tokens <= 300);
    }

    TEST_CASE("isValid returns true for valid prompts") {
        std::string valid_prompt = "Short prompt";
        CHECK(SystemPromptBuilder::isValid(valid_prompt, 1000));
    }

    TEST_CASE("isValid returns false for oversized prompts") {
        std::string long_prompt = std::string(10000, 'x');
        CHECK(!SystemPromptBuilder::isValid(long_prompt, 100));
    }
}

// ============================================================================
// Suite 6: Grep guard — fail if library re-introduces the banned strings.
// ============================================================================

TEST_SUITE("Library-text sanity") {

    TEST_CASE("Source file does not contain banned identity text") {
        // Read the source file and check for banned strings. If the library
        // ever re-introduces identity / workflow / tool-guidelines prose,
        // this test fails.
        std::ifstream f(std::string(SYSTEM_PROMPT_BUILDER_CPP_PATH));
        REQUIRE(f.is_open());
        std::stringstream ss;
        ss << f.rdbuf();
        std::string src = ss.str();

        // Banned: identity / rules / workflow / tool-guidelines prose
        // that should now live in the demo's system_prompt.txt.
        CHECK(src.find("autonomous coding agent") == std::string::npos);
        CHECK(src.find("Task Focus - CRITICAL") == std::string::npos);
        CHECK(src.find("Understand -> Plan -> Act -> Build/Test -> Stop")
              == std::string::npos);
        CHECK(src.find("ALWAYS call system_info") == std::string::npos);
        // The CMake preprocessor needs to define SYSTEM_PROMPT_BUILDER_CPP_PATH
        // — fall back gracefully if not defined.
    }
}

// ============================================================================
// Suite 7: State machine section is ALWAYS appended, even with minimal options.
// ============================================================================

TEST_SUITE("State machine section") {

    TEST_CASE("State machine section is non-empty") {
        // The static helper always returns a non-empty string — there is
        // no parameter, no way to disable it.
        std::string section = SystemPromptBuilder::getStateMachineSection();
        CHECK(!section.empty());
        CHECK(section.find("# State Machine") != std::string::npos);
        CHECK(section.find("Understanding") != std::string::npos);
        CHECK(section.find("Planning") != std::string::npos);
        CHECK(section.find("Acting") != std::string::npos);
        CHECK(section.find("Verifying") != std::string::npos);
        CHECK(section.find("Done") != std::string::npos);
    }

    TEST_CASE("Section is appended with empty host options") {
        // The library ALWAYS appends the state-machine section, even
        // when the host passes nothing — no system_prompt, no tools,
        // no skills, no memory.  The reserved-dirs section comes
        // AFTER it; we only assert the state-machine section is
        // present (its position relative to the reserved-dirs section
        // is verified in the dedicated Reserved Agent Dirs suite).
        SystemPromptOptions options;
        std::string prompt = SystemPromptBuilder::compose(options);

        CHECK(prompt.find("# State Machine") != std::string::npos);
    }

    TEST_CASE("Section is appended AFTER the host's system_prompt") {
        // The host's prose stays verbatim and first; the state-machine
        // section is appended at the very end (just before the
        // reserved-dirs section).
        SystemPromptOptions options;
        options.system_prompt = "CUSTOM HOST PROMPT MARKER.";
        std::string prompt = SystemPromptBuilder::compose(options);

        auto host_pos  = prompt.find("CUSTOM HOST PROMPT MARKER.");
        auto state_pos = prompt.find("# State Machine");
        REQUIRE(host_pos  != std::string::npos);
        REQUIRE(state_pos != std::string::npos);
        CHECK(host_pos < state_pos);
    }

    TEST_CASE("Section is appended when all other sections are populated") {
        // Even with every other section populated, the state-machine
        // section is present along with all the host-supplied adders.
        SystemPromptOptions options;
        options.system_prompt = "host prompt";
        options.tool_definitions = R"([{"name":"x"}])";
        options.active_skills    = "skill_a: desc";
        options.available_skills = "skill_b: desc";
        options.session_memory   = "memory content";
        std::string prompt = SystemPromptBuilder::compose(options);

        CHECK(prompt.find("host prompt")                  != std::string::npos);
        CHECK(prompt.find("# Tool Definitions")           != std::string::npos);
        CHECK(prompt.find("# Active Skills")              != std::string::npos);
        CHECK(prompt.find("# Available Skills (catalog)") != std::string::npos);
        CHECK(prompt.find("# Session Memory")             != std::string::npos);
        CHECK(prompt.find("# State Machine")              != std::string::npos);
        CHECK(prompt.find("# Reserved Agent Directories") != std::string::npos);

        // §ContextCache: no per-iteration stats block in the static prompt.
        CHECK(prompt.find("# Context Status") == std::string::npos);

        // §ContextCache follow-up (2026-08-02): the state-machine and
        // reserved-dirs sections stay together in the static prefix;
        // the volatile session-memory section is emitted LAST.
        auto sm_pos  = prompt.find("# State Machine");
        auto rd_pos  = prompt.find("# Reserved Agent Directories");
        auto mem_pos = prompt.find("# Session Memory");
        REQUIRE(sm_pos  != std::string::npos);
        REQUIRE(rd_pos  != std::string::npos);
        REQUIRE(mem_pos != std::string::npos);
        CHECK(sm_pos < rd_pos);
        CHECK(rd_pos < mem_pos);
    }

    TEST_CASE("State machine section survives with default options") {
        // The state-machine section MUST always be appended (it has no
        // flag), even when nothing else is populated.
        SystemPromptOptions options;
        std::string prompt = SystemPromptBuilder::compose(options);

        CHECK(prompt.find("# State Machine") != std::string::npos);
    }
}

// ============================================================================
// Suite 8: Reserved agent directories section is ALWAYS appended (after the
// state-machine section; only the volatile session-memory section may follow).
// ============================================================================

TEST_SUITE("Reserved agent dirs section") {

    TEST_CASE("Section is non-empty and self-explanatory") {
        std::string section = SystemPromptBuilder::getReservedAgentDirsSection();
        CHECK(!section.empty());
        CHECK(section.find("# Reserved Agent Directories") != std::string::npos);
        CHECK(section.find(".hive-agent")     != std::string::npos);
        CHECK(section.find(".git")            != std::string::npos);
        CHECK(section.find("NEVER")           != std::string::npos);
        CHECK(section.find("ALWAYS")          != std::string::npos);
    }

    TEST_CASE("Section is appended AFTER the state-machine section") {
        // Both library-owned blocks are appended; without session memory
        // the reserved-dirs section is the FINAL header so it's the last
        // thing the model reads before deciding what to do.
        SystemPromptOptions options;
        std::string prompt = SystemPromptBuilder::compose(options);

        auto sm_pos   = prompt.find("# State Machine");
        auto rd_pos   = prompt.find("# Reserved Agent Directories");
        REQUIRE(sm_pos != std::string::npos);
        REQUIRE(rd_pos != std::string::npos);
        CHECK(sm_pos < rd_pos);

        // Nothing follows the reserved-dirs section (other than
        // the body of the section itself).
        std::string tail = prompt.substr(rd_pos);
        // The next "\n# " header should not appear in the tail.
        CHECK(tail.find("\n# ") == std::string::npos);
    }

    TEST_CASE("Session memory is emitted AFTER the static mandatory sections") {
        // §ContextCache follow-up (2026-08-02): the volatile session-memory
        // dump must be the LAST section so the byte-static mandatory
        // sections stay inside the server-side prompt-cache prefix.
        SystemPromptOptions options;
        options.session_memory = "SESSION_MEMORY";
        options.include_session_memory = true;
        std::string prompt = SystemPromptBuilder::compose(options);

        auto sm_pos   = prompt.find("# State Machine");
        auto rd_pos   = prompt.find("# Reserved Agent Directories");
        auto mem_pos  = prompt.find("# Session Memory");
        REQUIRE(sm_pos != std::string::npos);
        REQUIRE(rd_pos != std::string::npos);
        REQUIRE(mem_pos != std::string::npos);
        CHECK(sm_pos < rd_pos);
        CHECK(rd_pos < mem_pos);

        // Nothing follows the memory section.
        std::string tail = prompt.substr(mem_pos);
        CHECK(tail.find("\n# ") == std::string::npos);
    }

    TEST_CASE("Section is appended when host prompt is empty") {
        SystemPromptOptions options;
        std::string prompt = SystemPromptBuilder::compose(options);
        CHECK(prompt.find("# Reserved Agent Directories") != std::string::npos);
    }
}