/**
 * @file hive-brain/AgentConfig.h
 * @brief Configuration passed to Agent's constructor.
 *
 * Every file path and behaviour flag is injectable here. The library does not
 * read or write `$HOME/.daedalus/...` implicitly; it uses what you pass (or the
 * defaults the helpers in <hive-brain/default_paths.h> provide).
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "types/ThinkingLevel.h"

namespace hive {

struct AgentConfig {
    // ----- Identity -----
    std::filesystem::path project_root;

    // §Phase 8.8 (2026-07-12): per-agent namespace for runtime
    // state. Empty string = the historical default (no namespace,
    // data lives under `<project_root>/.daedalus/`). Non-empty =
    // data lives under `<project_root>/.daedalus-<name>/`, e.g.
    // `.daedalus-bob/`. This allows multiple Agent instances in the
    // same project to have isolated `messages.json` /
    // `memory.json` without trampling each other, and enables
    // operations like "clone Bob into Alice" via a folder copy.
    //
    // §Phase 12 (rename): the legacy `.daedalus[-<name>]` paths are
    // still honoured as a read-only fallback for one release cycle.
    //
    // Validation rules (apply to non-empty values):
    //   - Length 1..64
    //   - Match `^[A-Za-z0-9_.-]+$`
    //   - May NOT start with '.' (hidden dir / parent-relative escape)
    //   - May NOT start with '-' (would look like a CLI flag)
    //   - May NOT contain '/', '\\', whitespace, or NUL
    //
    // Agent::initialize() validates and throws
    // std::invalid_argument on a bad name; you can also call
    // is_valid_agent_name() yourself first.
    //
    // ORTHOGONAL to daedalus_home (Phase 8.5): daedalus_home is the
    // config-source axis (settings.json, mcp/), agent_name is the
    // runtime-state axis (messages.json, memory.json). They don't
    // combine into a single path.
    //
    // What gets namespaced: messages.json, memory.json, session files.
    // What does NOT: settings.json (Phase 8.5 already covers this),
    // skills/, mcp/servers.json (project-shared).
    std::string agent_name;

    // ----- Model selection -----
    // Either set model_name (looked up in settings.json) OR set api_key+api_host
    // explicitly to skip the settings.json lookup.
    std::string model_name;        // empty = use settings.json default
    std::string api_key;           // empty = use settings.json
    std::string api_host;          // empty = use settings.json
    std::string chat_endpoint;     // empty = "/v1/chat/completions"
    int max_context_size = 0;      // 0 = use model's setting from settings.json

    // When true, the explicit fields above are authoritative: the
    // settings store's model entries are NEVER consulted for resolution
    // (even when model_name matches a settings key) and the store's
    // default_model_name is never auto-adopted. For hosts that own the
    // model registry themselves (e.g. HiveMind's worker models.json) and
    // inject credentials per spawn — prevents a per-agent settings store
    // from becoming a shadow registry that silently diverges.
    bool explicit_model_wins = false;

    // ----- Filesystem overrides -----
    // All optional; helpers in <hive-brain/default_paths.h> compute sensible defaults.
    //
    // §Phase 8.5 (2026-07-10): each Agent can have its own hive-brain home.
    // If set, this is the base for all derived paths below (settings,
    // mcp_servers, mcp_env, memory). If a derived field is explicitly
    // set, it wins over the daedalus_home default. Two Agents in one
    // process can now read different settings.json files, use different
    // MCP servers, and store memory in different places.
    //
    // §Phase 10+: the empty default is $DAEDALUS_HOME (this library's
    // env var) or $HOME/.local/share/hive-brain (XDG default).
    std::filesystem::path daedalus_home;      // empty = $DAEDALUS_HOME or XDG default
    // §Plan.md v0.5.0 (settled): the per-field *_path overrides
    // (`settings_path`, `mcp_servers_path`, `mcp_env_path`, `memory_dir`,
    // `log_file`) are GONE. Hosts that want a custom path for any of these
    // supply it via the SettingsStore (or pass it directly to the relevant
    // subsystem); the AgentConfig struct now has a single source of truth,
    // `daedalus_home`.

    // ----- Behaviour flags -----
    // §BUG-006: auto_approve is now tri-state. Unset (std::nullopt) = legacy
    // behaviour: fall back to the SettingsStore `auto_approve` row. Set =
    // the host is authoritative: the store value is ignored entirely, so a
    // host can force approval prompts ON even when another front-end left
    // `auto_approve=true` in the shared store. (Previously this was a plain
    // bool that could only ever *widen* auto-approve, never restrict it.)
    std::optional<bool> auto_approve;
    bool memory_enabled = true;
    bool clear_memory = false;
    // §ContextCache follow-up (2026-08-02): when false, the Session
    // Memory section is omitted from the composed system prompt while
    // memory collection continues. Lets hosts that surface memory
    // through their own trailing message keep the system prompt fully
    // byte-static for server-side prompt caches.
    bool memory_in_prompt = true;

    // §Phase 11: MCP server integration. Default = true (load MCP servers).
    // Set to false to completely disable MCP — no McpServerManager is
    // constructed, no MCP tools are available, no MCP status in the
    // system prompt. Useful for sandboxed agents or hosts that don't
    // want external tool integrations.
    bool mcp_enabled = true;
    int  max_agent_iterations = 300;
    std::size_t max_tool_result_size = 100000;
    std::size_t max_request_size  = 4000000;
    std::size_t max_response_size = 5000000;
    int  worker_threads = 0;       // 0 = hardware_concurrency

    // §Phase 8.7: sampling temperature. Default -1.0 means "use
    // model's temperature from settings.json". Explicit values (0.0–2.0)
    // override the model.  0.0 = deterministic (right for code agents).
    double temperature = -1.0;

    // Thinking/reasoning level selection (2026-08-01). Default = send
    // nothing and keep the provider/model default. Support is per-model:
    // Kimi K3 ids accept Low/High/Max, kimi-k2.5/k2.6 and MiniMax-M3
    // accept None (thinking off); every other model ignores the setting.
    // Unsupported selections are never sent to the provider. See
    // Agent::supported_thinking_levels() for a per-model capability query.
    ThinkingLevel thinking_level = ThinkingLevel::Default;

    // §Phase 9.8: SSE streaming. Default = true (token-by-token,
    // webui / CLI REPL get progressive text). Set to false for
    // batch-style hosts (test fixtures, deterministic-replay tools).
    bool stream = true;

    // §Phase 11: MCP server integration. Developer controls MCP loading per-Agent.
    // §Phase 2.7: removed stale Mode enum. The Agent has no modes — manager/
    // sub-agent patterns are host concerns. See demos/daedalus-manager/ and
    // demos/daedalus-subagent/ for composition patterns.

    // ----- Skills (§Phase 9.7) -----
    // Developer-controlled Skill loading. Both default to empty —
    // no skills are loaded unless the host explicitly opts in.
    //
    //   skills_dirs: directories to scan for SKILL.md. Each
    //                immediate subdirectory is treated as one skill
    //                and is loaded + cached. Paths are expanded
    //                with ~ and $VAR (matching SkillManager).
    //   skills:      names to *activate* (visible in the system
    //                prompt's Active Skills section). Loaded from
    //                any dir already in skills_dirs.
    //
    // Example (webui demo loads the user's global skills):
    //   cfg.skills_dirs = { hive::Paths::global().global_skills_dir() };
    //   cfg.skills      = { "firefox", "memory-leak" };
    std::vector<std::filesystem::path> skills_dirs;
    std::vector<std::string>           skills;

    // §Phase 15: per-skill / per-server enable/disable. Empty = all
    // enabled (back-compat). Names match the metadata.name of a skill
    // (from SKILL.md YAML) or the server key in mcp_servers.json.
    //
    //   mcp_servers_disabled: McpServerManager skips these servers at
    //     load_config(); their tools never enter the tool registry, and
    //     they don't appear in the # Active MCP Servers block.
    //   skills_disabled: SkillManager hides these skills from
    //     get_all_cached_skills() / get_active_skills(); the load_skill
    //     tool returns "skill disabled by host config" if the agent tries
    //     to activate one at runtime.
    //   tools_disabled: Tool names that are hidden from the LLM and rejected
    //     if invoked. Exact tool names (including host-registered tools) and
    //     the built-in names such as "execute_bash" or "discover_skills".
    std::set<std::string> mcp_servers_disabled;
    std::set<std::string> skills_disabled;
    std::set<std::string> tools_disabled;

    // ----- Correlation -----
    // Opaque session id (e.g. web UI session uuid). Propagated to log lines.
    std::string session_id;

    // ----- Role prompt (§Phase 6) -----
    // Host-supplied persona / role description. The library prepends it
    // to the composed system prompt so it dominates the agent's identity.
    // Empty = use the library default role (no override).
    std::string system_prompt;

    // ----- HTTP identity -----
    // User-Agent sent on LLM API requests. Empty = library default
    // ("hive-brain/1.0"). Hosts embedding the library should set this to
    // identify themselves to the provider, e.g. "icarus/1.2.0".
    std::string user_agent;
};

}  // namespace hive