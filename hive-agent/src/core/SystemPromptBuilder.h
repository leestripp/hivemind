/**
 * @file SystemPromptBuilder.h
 * @brief §Phase 14 — system prompt composer.
 *
 * The library NEVER writes identity / rules / workflow / build-system /
 * tool-guidelines prose. Those belong in the host's `system_prompt.txt`
 * (or whatever the demo passes to AgentConfig::system_prompt).
 *
 * What the library DOES:
 *   1. Take the host's system_prompt verbatim as the first section.
 *   2. Append a fenced JSON block of the live tool definitions.
 *   3. Append the active-skills list (names + short info).
 *   4. Append the available-skills catalog (names + descriptions).
 *   5. Append the session memory fact dump (opt-in).
 *
 * §ContextCache: the composed prompt is deliberately BYTE-STATIC for
 * the whole session — MiniMax / Kimi server-side prompt caches match
 * on a stable prefix. Per-iteration runtime stats (token usage,
 * iteration counter, clock) are NOT part of this prompt; they are
 * carried as a transient trailing message (src/api/ContextStatus.h).
 *
 * Anything the library can't generate itself (the host's prose) is the
 * host's job. See Agent.md Phase 14 for the full plan and rationale.
 */
#pragma once

#include <string>
#include <vector>

// ============================================================================
// Options — host-supplied state, NOT library prose.
// ============================================================================

struct SystemPromptOptions {
    // §Phase 14: required. The host's full system prompt (identity, rules,
    // workflow, build conventions, persona — whatever the host wants the
    // agent to be). Inserted verbatim as the FIRST section.
    std::string system_prompt;

    // §Phase 14: optional. A 1-line note about the `system_info` tool, so
    // the host can choose to remind the agent to call it (or not). Empty =
    // not included.
    std::string system_info_note;

    // The library-generated, dynamic sections (all formatted by the library):

    // JSON Schema dump of the live tool surface (ToolHandler::get_tool_definitions()).
    std::string tool_definitions;

    // §Phase 14: Active MCP server status (name + transport + tool list).
    // Pre-formatted prose — caller runs mcp_manager_->list_connected_servers() and
    // mcp_manager_->get_all_tools() to build it. The MCP **tools** also appear in
    // tool_definitions (above); this section is the human-readable "what servers
    // are connected and what they offer" summary.
    std::string mcp_servers;

    // Currently-loaded skills — SUMMARY (table of name + description).
    // Full SKILL.md content is fetched on demand via the get_skill_context tool.
    std::string active_skills;

    // Catalog of available skills (name + description, NOT full SKILL.md).
    // Should EXCLUDE active skill names to avoid duplication with active_skills.
    std::string available_skills;

    // Fact dump from SessionMemory (SessionMemory::get_system_prompt_addition()).
    std::string session_memory;

    // Feature flags.
    bool include_session_memory = true;
    bool include_system_info_note = true;  // applies when system_info_note is non-empty

    SystemPromptOptions() = default;
};

// ============================================================================
// SystemPromptBuilder — section formatters + composer
// ============================================================================

class SystemPromptBuilder {
public:
    // ---- composer (the one entry point the library exposes) ----

    /// §Phase 14: assemble host prompt + library adders. The host's
    /// `system_prompt` is always the first section; everything else is
    /// appended in a fixed order (see Agent.md §14.4).
    static std::string compose(const SystemPromptOptions& options);

    // ---- section formatters (all take content as parameter; no prose) ----

    /// `# Session Memory\n<memory>` — empty input returns empty string.
    static std::string getSessionMemorySection(const std::string& memory);

    /// `# Active Skills\n<skills>` — empty input returns empty string.
    static std::string getActiveSkillsSection(const std::string& skills);

    /// `# Available Skills\n<catalog>\n\n" + load_skill instruction.
    static std::string getAvailableSkillsSection(const std::string& skills);

    /// `# Tool Definitions (JSON Schema)\n\`\`\`json\n<defs>\n\`\`\``.
    static std::string getToolDefinitionsSection(const std::string& tool_defs);

    /// `# Active MCP Servers\n<status>` — names, transports, and tool list per server.
    static std::string getMcpServersSection(const std::string& status);

    /// `# State Machine\n<section>` — ALWAYS appended last.  Tells the agent that it
    /// owns the state machine (Idle / Understanding / Planning /
    /// Acting / Verifying / Done) and must update it via its tool
    /// calls as it works through a turn.  Returns a static,
    /// non-empty string — there is no host option to disable it.
    static std::string getStateMachineSection();

    /// §NarrationLeak: ALWAYS appended.  Posted replies must carry only
    /// the answer — never session-machinery narration (context %, cache,
    /// compression) or internal bookkeeping ("closing", "banked").
    static std::string getReplyDisciplineSection();

    /// `# Reserved Agent Directories (.hive-agent*)` — ALWAYS appended
    /// last (after the state-machine section).  Explains that the
    /// `.hive-agent/`, `.hive-agent/default/`, and
    /// `.hive-agent/<name>/` folders in the project root are owned
    /// by the agent runtime and must NEVER be deleted, renamed, edited,
    /// or moved by the model.  Returns a static, non-empty string —
    /// there is no host option to disable it.
    static std::string getReservedAgentDirsSection();

    // ---- utility ----

    static size_t estimateTokens(const std::string& text);
    static bool   isValid(const std::string& prompt, size_t max_tokens = 160000);

private:
    static std::string combineSections(const std::vector<std::string>& sections);
};