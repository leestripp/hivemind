/**
 * @file SystemPromptBuilder.cpp
 * @brief §Phase 14 — host prompt + library adders.
 *
 * See Agent.md Phase 14 for the design rationale. The library's job is
 * strictly mechanical: take the host's text verbatim and append sections
 * that the host couldn't have written itself (live tool list, live skill
 * list, session memory, runtime stats). No prose, no identity, no rules.
 */

#include "SystemPromptBuilder.h"

#include <pwd.h>
#include <ctime>
#include <iomanip>
#include <sstream>

// ============================================================================
// §Phase 14: SystemPromptOptions default-constructs with the in-class
// defaults declared in the header. The constructor used to fetch a username
// from the OS passwd database, but the library no longer writes that into
// the composed prompt — the host's system_prompt can include username if
// it wants to. Kept the comment as a tombstone; nothing else to do here.
// ============================================================================

// ============================================================================
// Utility
// ============================================================================

size_t SystemPromptBuilder::estimateTokens(const std::string& text) {
    // Rough estimate: 1 token ≈ 4 bytes (matches what most providers charge).
    return text.size() / 4;
}

bool SystemPromptBuilder::isValid(const std::string& prompt, size_t max_tokens) {
    return estimateTokens(prompt) <= max_tokens;
}

std::string SystemPromptBuilder::combineSections(const std::vector<std::string>& sections) {
    std::string result;
    for (const auto& section : sections) {
        if (!section.empty()) {
            if (!result.empty()) {
                result += "\n";
            }
            result += section;
        }
    }
    return result;
}

// ============================================================================
// Section formatters — all take content as parameter. NO library prose.
// ============================================================================

std::string SystemPromptBuilder::getSessionMemorySection(const std::string& memory) {
    if (memory.empty()) return "";
    return "# Session Memory\n" + memory;
}

std::string SystemPromptBuilder::getActiveSkillsSection(const std::string& skills) {
    if (skills.empty()) return "";
    return "# Active Skills\n" + skills;
}

std::string SystemPromptBuilder::getAvailableSkillsSection(const std::string& skills) {
    if (skills.empty()) return "";
    return std::string("# Available Skills (catalog)\n\n")
         + "The following skills are available. If your task seems related to one, "
         + "use the load_skill tool to activate it before proceeding.\n\n"
         + skills;
}

std::string SystemPromptBuilder::getToolDefinitionsSection(const std::string& tool_defs) {
    if (tool_defs.empty()) return "";
    std::ostringstream oss;
    oss << "# Tool Definitions (JSON Schema)\n```json\n" << tool_defs << "\n```";
    return oss.str();
}

std::string SystemPromptBuilder::getMcpServersSection(const std::string& status) {
    if (status.empty()) return "";
    return "# Active MCP Servers\n" + status;
}

std::string SystemPromptBuilder::getStateMachineSection() {
    // §State Machine: ALWAYS appended.  The agent is the only one that
    // knows when it is done — and when it is NOT done (e.g. verification
    // failed → switch back to Understanding).  We tell it explicitly so
    // it never stalls in a state, and so the host UI / sink can render
    // the current stage correctly.  The ONLY way to change state is the
    // `set_status` tool — there is no implicit per-tool-call state field.
    return
        "# State Machine\n"
        "\n"
        "You drive a state machine. On every transition the library\n"
        "notifies the host through its `on_state_change` callback; the\n"
        "host UI uses those events to render the current stage.\n"
        "\n"
        "**How to change state:** call the `set_status` tool with\n"
        "`{\"state\": \"<Name>\"}` whenever you enter a new phase. This is\n"
        "the ONLY mechanism — tool calls do not carry an implicit state,\n"
        "and the host cannot see your phase unless you call `set_status`.\n"
        "\n"
        "States:\n"
        "\n"
        "| State | Meaning |\n"
        "|-------|---------|\n"
        "| `Understanding` | Reading the request, parsing input, gathering context. |\n"
        "| `Planning` | Designing the approach, choosing which tools to call. |\n"
        "| `Acting` | Running tools (file edits, bash, MCP, etc.). |\n"
        "| `Verifying` | Compiling, testing, inspecting output — this is a decision gate. |\n"
        "| `Documenting` | Updating README, CHANGELOG, comments, docs after verification. |\n"
        "| `Done` | Turn complete — emit your final assistant message, then call the `stop` tool. |\n"
        "| `Cancelled` | Host cancelled the turn; stop work and ack. |\n"
        "| `Error` | Unrecoverable error; report to host and stop. |\n"
        "\n"
        "Runtime-owned states (informational — the library sets these\n"
        "itself; never pass them to `set_status`):\n"
        "\n"
        "| State | Meaning |\n"
        "|-------|---------|\n"
        "| `Idle` | Constructed / resting between turns — no work in flight. |\n"
        "| `AwaitingApproval` | A bash command is waiting for host approval. |\n"
        "| `Compressing` | Context compaction in flight. |\n"
        "\n"
        "**`set_status` never exits the agent loop** — no state does, not\n"
        "even `Done`. The turn only ends when you call the `stop` tool\n"
        "(or the host cancels). `set_status` exists purely to keep the\n"
        "host UI stage chip in sync.\n"
        "\n"
        "Workflow:\n"
        "\n"
        "1. **Understand** the request and the codebase.\n"
        "2. **Plan** the approach; create or update todos so the work is tracked.\n"
        "3. **Act** — make the code changes.\n"
        "4. **Verify** — build, run tests, inspect results. This is a gate, not a\n"
        "   formality. Look at the actual output and decide pass or fail.\n"
        "5. **Document** only if the change affects user-visible behaviour. Update\n"
        "   README, CHANGELOG, comments, or tool descriptions as needed.\n"
        "6. **Done** — only when verification passed and docs are complete.\n"
        "\n"
        "Rules:\n"
        "\n"
        "1. **You are the only one who knows when the work is truly done.**\n"
        "   Do not stay in `Acting` after the last tool result — call\n"
        "   `set_status` to switch to `Verifying` and actually inspect the\n"
        "   results.\n"
        "2. **If verification fails, loop back.** Do not go to `Done`.\n"
        "   - If you need more context, call `set_status` with `Understanding`.\n"
        "   - If you know what to fix, call `set_status` with `Planning`,\n"
        "     add new todos, then continue.\n"
        "3. **If verification passes but docs need updating, switch to\n"
        "   `Documenting`.** Only go to `Done` after docs are complete.\n"
        "4. **Call `set_status` on every phase change.** The host UI shows\n"
        "   stale stage chips until you do.\n"
        "5. **Never stay in `Acting` forever.** If you have no more tools to\n"
        "   call, transition to `Verifying` (and then `Done` or `Documenting`)\n"
        "   or back to `Planning` if more work is needed.\n"
        "6. **Always end with `Done`, then `stop`.** Call `set_status` with\n"
        "   `Done` immediately before your final assistant message, then call\n"
        "   the `stop` tool — `set_status` alone does not end the loop, and\n"
        "   without `stop` the turn never terminates.\n";
}

std::string SystemPromptBuilder::getReservedAgentDirsSection() {
    // §Reserved Agent Dirs: ALWAYS appended last.  Tells the agent
    // that .daedalus* folders are reserved runtime state — same
    // contract as .git/ — so it never deletes / renames / edits /
    // moves them.  Without this, a well-meaning "clean up junk
    // directories" task will silently nuke the agent's memory and
    // the user's session scratch state.
    return
        "# Reserved Agent Directories (.daedalus*)\n"
        "\n"
        "The following directories in the project root are owned by the\n"
        "agent runtime. They are HIDDEN system folders — treat them with\n"
        "the same care as `.git/`.\n"
        "\n"
        "| Directory | Purpose |\n"
        "|-----------|---------|\n"
        "| `.daedalus/` | Base directory for all agent runtime state. Do not touch. |\n"
        "| `.daedalus/hive-brain/` | Default agent's persistent state. Memory, session scratch, per-project config, skills cache. |\n"
        "| `.daedalus/daedalus-<id>/` | Per-agent / per-session state when the host namespaces agents. `<id>` is a hex / session id; the contents are the same as `.daedalus/hive-brain/`. |\n"
        "\n"
        "NEVER:\n"
        "\n"
        "1. **Delete** any `.daedalus*` directory. Doing so wipes the\n"
        "   agent's memory and all session scratch — unrecoverable.\n"
        "2. **Rename** or **move** them.\n"
        "3. **Edit files inside them directly** with `modify_file` /\n"
        "   `edit_file_patch` / `create_file` unless the user has\n"
        "   EXPLICITLY asked you to (and even then, ask first).\n"
        "4. Include them in any \"clean up junk directories\" or\n"
        "   \"remove cruft\" task. They are not junk.\n"
        "5. Suggest that the user delete them to \"fix\" a problem.\n"
        "   They are not the cause of any problem you can observe.\n"
        "\n"
        "ALWAYS:\n"
        "\n"
        "1. Treat them as **out-of-bounds** during normal work — like a\n"
        "   `.git/` folder: read what's relevant via the agent's own\n"
        "   tools (`memory_*`, `get_skill_context`, etc.) and never\n"
        "   touch the directory tree directly.\n"
        "2. If the user asks you to do something that would affect a\n"
        "   `.daedalus*` directory, **stop and ask** before doing it.\n";
}

// ============================================================================
// compose() — the only composer the library exposes.
//
// §Phase 14: takes the host's system_prompt verbatim as the first section,
// then appends the dynamic adders in a fixed order. The host text is never
// inspected, parsed, modified, or augmented by the library.
// ============================================================================

std::string SystemPromptBuilder::compose(const SystemPromptOptions& options) {
    std::vector<std::string> sections;

    // 1. The host's full system prompt — verbatim. ALWAYS first.
    if (!options.system_prompt.empty()) {
        sections.push_back(options.system_prompt);
    }

    // 2. Optional 1-line note about system_info (host decides policy).
    if (options.include_system_info_note && !options.system_info_note.empty()) {
        sections.push_back(options.system_info_note);
    }

    // 3. Active MCP servers — names + transports + tools. Built by the
    //    caller from McpServerManager::list_connected_servers() and
    //    get_all_tools(). The MCP **tools** also appear in the next section
    //    (tool_definitions JSON schema); this is the human-readable "what
    //    servers are connected and what they offer" summary.
    if (!options.mcp_servers.empty()) {
        sections.push_back(getMcpServersSection(options.mcp_servers));
    }

    // 4. Tool JSON schema — the live tool surface.
    if (!options.tool_definitions.empty()) {
        sections.push_back(getToolDefinitionsSection(options.tool_definitions));
    }

    // 5. Active skills (currently loaded) — SUMMARY (table of name + description).
    //    Full SKILL.md content is fetched on demand via the get_skill_context tool.
    if (!options.active_skills.empty()) {
        sections.push_back(getActiveSkillsSection(options.active_skills));
    }

    // 6. Available skills catalog (names + descriptions, NOT full SKILL.md).
    //    Caller is responsible for filtering out active skills to avoid duplication.
    if (!options.available_skills.empty()) {
        sections.push_back(getAvailableSkillsSection(options.available_skills));
    }

    // 7. State machine — ALWAYS appended. Non-empty by construction;
    //    there is no host option to disable it.  This is so the host
    //    can be sure the prompt always explains the state machine and
    //    the agent is instructed to set it as it works.
    sections.push_back(getStateMachineSection());

    // 8. Reserved agent directories — ALWAYS appended.  Tells the agent
    //     that .daedalus* folders are owned by the runtime and must
    //     never be deleted / renamed / edited.
    //
    // §ContextCache: the composed prompt must stay BYTE-STATIC across
    // the whole session — MiniMax / Kimi server-side prompt caches
    // match on a stable prefix, so no per-iteration data (token
    // counters, timestamps) may live here. The per-iteration Context
    // Status block is carried as a transient trailing message instead
    // (see src/api/ContextStatus.h and LLMClient::inject_context_stats).
    sections.push_back(getReservedAgentDirsSection());

    // 9. Session memory (opt-in) — ALWAYS last.
    //
    // §ContextCache follow-up (2026-08-02, icarus patch review): the
    // memory dump is the most volatile section — its stats header
    // changes on almost every run. Server-side prompt caches stop at
    // the first differing byte, so emitting memory LAST keeps slots 7-8
    // (~2.4k tokens of byte-static mandatory text) inside the cacheable
    // prefix regardless of memory churn; only this final section misses.
    if (options.include_session_memory && !options.session_memory.empty()) {
        sections.push_back(getSessionMemorySection(options.session_memory));
    }

    return combineSections(sections);
}

