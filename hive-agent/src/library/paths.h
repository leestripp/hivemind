/**
 * @file hive-brain/paths.h
 * @brief XDG-compliant filesystem location resolution (Plan.md Phase 1).
 *
 * The library maintains two storage tiers:
 *
 *   - **User tier** — per-user state, shared across every project.
 *     LLM API keys, MCP server registry, globally-installed skill
 *     packs, logs, history, cache. Resolves under
 *     `$XDG_CONFIG_HOME/hive-brain/`, `$XDG_DATA_HOME/hive-brain/`,
 *     `$XDG_STATE_HOME/hive-brain/`, `$XDG_CACHE_HOME/hive-brain/`.
 *
 *   - **Project tier** — per-project state, namespaced by
 *     `agent_name`. Long-term memory, session transcripts, scratch.
 *     Lives under `<project_root>/.daedalus[-<agent_name>]/`.
 *
 * This header covers the user tier only. See
 * `include/hive-brain/project_paths.h` for the project tier.
 *
 * **Cardinal rule:** memory MUST live in the project tier. No code
 * path is permitted to write to any `$XDG_*` path under a `memory*`
 * directory. Enforced by `tests/test_two_tier_rule.cpp`.
 *
 * **Backward compat:** existing user code calling
 * If you used `hive::default_settings_path()` or
 * `hive::default_mcp_servers_path()` in existing code, replace those
 * with `hive::SettingsStore::instance()` (for the store) or
 * compiling — those helpers now forward into `Paths::global()`.
 * The only behaviour change is: when an `$XDG_*` variable is set in
 * the environment, the helpers honour it (previously they did not).
 */
#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace hive {

/**
 * @brief Resolves user-tier filesystem paths under XDG-compliant
 *        locations (Plan.md §3.2).
 *
 * Obtain the canonical implementation via `Paths::global()`. Hosts
 * can inject a custom implementation for testing or sandboxing via
 * `Paths::set_override()`.
 *
 * Thread-safety: `global()` is safe to call from any thread after
 * first initialisation. `set_override()` is NOT thread-safe and must
 * be called before any `Agent` is constructed.
 */
class Paths
{
public:
	virtual ~Paths() = default;

	// --- User tier (writable, per-user, shared across projects) ---

	/// User "root" — parent of `config/`, `data/`, `state/`, `cache/`
	/// subdirs. The single-root convenience override
	/// (`$HIVE_AGENT_HOME` / `$CODEMAXLIB_HOME`) lands here when set.
	/// Priority: $HIVE_AGENT_HOME → $XDG_DATA_HOME/hive-brain →
	/// $HOME/.daedalus (LEGACY) → $HOME/.local/share/hive-brain.
	virtual std::filesystem::path user_root() const = 0;

	/// `$XDG_CONFIG_HOME/hive-brain` (default `~/.config/hive-brain`).
	/// Holds `config.json`, `mcp/servers.json`, `mcp/.env`.
	virtual std::filesystem::path user_config_root() const = 0;

	/// `$XDG_DATA_HOME/hive-brain` (default `~/.local/share/hive-brain`).
	/// Holds user-installed skill packs (NOT memory).
	virtual std::filesystem::path user_data_root() const = 0;

	/// `$XDG_STATE_HOME/hive-brain` (default `~/.local/state/hive-brain`).
	/// Holds logs, history, stats.
	virtual std::filesystem::path user_state_root() const = 0;

	/// `$XDG_CACHE_HOME/hive-brain` (default `~/.cache/hive-brain`).
	/// Holds regenerable caches (response replay, embeddings).
	virtual std::filesystem::path user_cache_root() const = 0;

	// --- System tier (read-only at runtime) ---

	/// `$XDG_DATA_DIRS/hive-brain` search path (default
	/// `/usr/local/share/hive-brain:/usr/share/hive-brain`).
	/// First entry is highest priority. Read-only shipped skill
	/// packs, default prompt templates.
	virtual std::vector<std::filesystem::path> system_data_dirs() const = 0;

	/// `$XDG_CONFIG_DIRS/hive-brain` search path (default
	/// `/etc/xdg/hive-brain`). Admin-editable system config.
	virtual std::vector<std::filesystem::path> system_config_dirs() const = 0;

	// --- Concrete global file paths (derived above) ---

	/// `user_config_root() / "config.json"` — LLM API keys + models.
	virtual std::filesystem::path config_file() const = 0;

	/// `user_config_root() / "mcp" / "servers.json"`.
	virtual std::filesystem::path mcp_servers_file() const = 0;

	/// `user_config_root() / "mcp" / ".env"`.
	virtual std::filesystem::path mcp_env_file() const = 0;

	/// `user_data_root() / "skills"`.
	virtual std::filesystem::path global_skills_dir() const = 0;

	/// `user_state_root() / "logs" / "hive-brain.log"`.
	virtual std::filesystem::path log_file() const = 0;

	/// `user_cache_root() / "responses"`.
	virtual std::filesystem::path response_cache_dir() const = 0;

	/// `$XDG_CONFIG_HOME/omninet/config.json` (default
	/// `~/.config/omninet/config.json`). OmniNet tooling expects its
	/// own config namespace; hive-brain exposes this so hosts can set it
	/// up. The default implementation uses the XDG config root.
	virtual std::filesystem::path omninet_config_file() const
	{
		return user_config_root() / "omninet" / "config.json";
	}

	/// `$XDG_DATA_HOME/omninet` (default `~/.local/share/omninet`).
	/// Runtime data directory used by OmniNet tools. The default
	/// implementation uses the XDG data root.
	virtual std::filesystem::path omninet_data_dir() const
	{
		return user_data_root() / "omninet";
	}

	/// Create the user-tier root directories on disk if they don't
	/// exist, applying Plan.md §5.1 permissions:
	///   - user_config_root()  → 0750  (group-readable)
	///   - user_data_root()    → 0750  (group-readable)
	///   - user_state_root()   → 0700  (private — may hold API keys
	///                                in logs, secrets in .env)
	///   - user_cache_root()   → 0700  (private — replay cache may
	///                                contain response bodies)
	///
	/// Idempotent — safe to call multiple times. No-op if directories
	/// already exist. Respects `$HOME/.daedalus/` legacy layout when
	/// `user_root()` resolves there (permission applied to that path).
	///
	/// Does NOT create subdirectories (logs/, mcp/, skills/);
	/// those are created on-demand by the subsystem writing the file.
	/// Hosts typically call this once during `Agent::initialize()`.
	virtual void ensure_directories() const = 0;

	// --- Singleton / test injection ---

	/// Returns the process-wide instance. First call resolves XDG
	/// from the environment; subsequent calls return the same
	/// implementation. Thread-safe.
	static const Paths& global() noexcept;

	/// Replace the singleton (testing / sandboxing). NOT thread-safe.
	/// Pass `nullptr` to reset to the default platform implementation.
	static void set_override( std::shared_ptr<Paths> p ) noexcept;

	/// Construct the default platform implementation. Exposed for
	/// hosts that want a non-singleton instance (e.g. multi-tenant).
	static std::unique_ptr<Paths> make_default();
};

/// Default factory — same as `Paths::make_default()`.
std::unique_ptr<Paths> make_default_paths();

}  // namespace hive
