/**
 * @file AgentImpl.cpp
 *
 * §Phase 1+: implements hive::Agent by wrapping the real subsystems.
 *
 * Wiring:
 *   AgentConfig ──> AppConfig ──> LLMClient
 *   LLMClient ──> AgentSink (already wired via LLMClient::set_sink)
 *   LLMClient::AgentState ──> hive::AgentState (map_from_internal)
 *   cancel() ──> LLMClient internal stop_token
 */
#include "AgentImpl.h"

#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <stop_token>
#include <utility>

#include "library/model_discovery.h"
#include "library/paths.h"
#include "core/settings.h"
#include "api/ProviderPlugin.h"    // §ProviderPlugins: endpoint fallback
#include "api/ProviderRegistry.h"  // §ProviderPlugins
#include "api/LLMClient.h"           // internal subsystem (src/api/)
#include "core/FileIO.h"             // for execute_tool FileIO construction
#include "core/Logger.h"             // LOG_ERROR for on_error file logging
#include "core/SessionMemory.h"      // internal subsystem (src/core/)
#include "core/SkillManager.h"       // internal subsystem (src/core/)
#include "tools/ToolHandler.h"       // internal subsystem (src/tools/)
#include "tools/ToolFactory.h"       // internal subsystem (src/tools/)
#include "tools/HostToolAdapter.h"   // adapter: HostTool → BaseTool
#include "tools/TodoListService.h"   // public Agent todo API backing
#include "LibraryToolContext.h"
#include "library/default_paths.h"   // default_settings_path()

namespace hive::detail {

namespace fs = std::filesystem;

// ===========================================================================
// Body — holds LLMClient. (Defined in AgentImpl.h so that
// std::unique_ptr<Body>'s destructor is visible at the AgentImpl dtor site.)
// ===========================================================================

// ---------------------------------------------------------------------------
// State mapping
// ---------------------------------------------------------------------------

namespace {

constexpr AgentState map_from_internal(::AgentState s) noexcept {
    switch (s) {
        case ::AgentState::UNDERSTAND:  return AgentState::Understanding;
        case ::AgentState::PLANNING:    return AgentState::Planning;
        case ::AgentState::ACT:         return AgentState::Acting;
        case ::AgentState::BUILD_TEST:  return AgentState::Verifying;
        case ::AgentState::DONE:        return AgentState::Done;
        case ::AgentState::CANCELLED:   return AgentState::Cancelled;
    }
    return AgentState::Error;
}

AppConfig make_app_config(const AgentConfig& cfg,
                          const hive::SettingsStore& store)
{
    AppConfig out;
    out.target_directory = cfg.project_root.empty()
                            ? std::filesystem::current_path()
                            : cfg.project_root;
    out.agent_name = cfg.agent_name;        // §Phase 8.8
    out.session_id = cfg.session_id;        // §ContextCache: prompt-cache key fallback
    if (!cfg.model_name.empty() && !cfg.explicit_model_wins) {
        auto m = store.resolve_model(cfg.model_name);
        if (m.has_value()) {
            out.base_url      = m->host;
            out.api_key       = m->api_key;
            out.model         = m->model_id;
            out.model_name    = cfg.model_name;     // settings key
            out.provider      = m->provider;        // §ProviderPlugins
            out.plugin        = m->plugin;          // §ProviderPlugins
            out.chat_endpoint = m->chat_endpoint;
            out.temperature   = m->temperature;  // §Phase 8.7
        }
    }
    // Fall back to explicit AgentConfig fields if settings didn't supply.
    if (out.base_url.empty()) out.base_url = cfg.api_host;
    if (out.api_key.empty())  out.api_key  = cfg.api_key;
    if (out.model.empty())    out.model    = cfg.model_name;
    if (out.model_name.empty()) out.model_name = cfg.model_name;
    if (out.chat_endpoint.empty())
        out.chat_endpoint = cfg.chat_endpoint.empty()
                                ? "/v1/chat/completions" : cfg.chat_endpoint;

    // §BUG-006: cfg.auto_approve is authoritative when the host sets it;
    // only when unset do we fall back to the SettingsStore row (legacy).
    if (cfg.auto_approve) {
        out.auto_mode = *cfg.auto_approve;
    } else {
        try { out.auto_mode = store.get_auto_approve(); }
        catch (...) { out.auto_mode = false; }
    }
    out.memory_disabled = !cfg.memory_enabled;
    out.memory_in_prompt = cfg.memory_in_prompt;
    out.system_prompt  = cfg.system_prompt;   // §Phase 6: host-supplied role
    if (!cfg.user_agent.empty()) out.user_agent = cfg.user_agent;  // §UA
    out.stream         = cfg.stream;            // §Phase 9.8: SSE streaming
    // §Phase 8.7: temperature resolution. Explicit cfg.temperature >= 0
    // wins over the model's value. Negative = use model's or default 0.0.
    if (cfg.temperature >= 0.0) {
        out.temperature = cfg.temperature;
    }
    // Thinking level (2026-08-01): host selection is passed through
    // verbatim; plugins decide per-model whether to honor it.
    out.thinking_level = cfg.thinking_level;
    return out;
}

}  // namespace

// ---------------------------------------------------------------------------
// AgentImpl
// ---------------------------------------------------------------------------

AgentImpl::AgentImpl(AgentConfig cfg, std::shared_ptr<AgentSink> sink)
    : cfg_(std::move(cfg))
    , sink_(std::move(sink))
    , body_(std::make_unique<Body>())
    , host_ctx_(std::make_shared<LibraryToolContext>(cfg_))
{
    // §v0.5.0: settings moved to SQLite. AgentConfig::settings_path /
    // mcp_servers_path / mcp_env_path / memory_dir are now deprecated
    // ([[deprecated]] in default_paths.h) — the library reads from
    // SettingsStore via cfg.daedalus_home. The legacy path-defaulting
    // block that used to live here (assigning those four fields from
    // cfg.daedalus_home or `default_*_path()` helpers) was deleted in
    // v0.4.2: those fields were set, never read.

    // §v0.5.0: wire directly to SettingsStore instead of SettingsManager.
    // Use per-agent DB if daedalus_home is set, otherwise the global singleton.
    // §B2 fix (v0.5.1): for_home() and instance() now return by value;
    // emplace into the optional so the stored copy is stable for the
    // lifetime of Body (and LLMClient, which takes a pointer to it).
    if (!cfg_.daedalus_home.empty()) {
        body_->store.emplace(hive::SettingsStore::for_home(cfg_.daedalus_home));
    } else {
        body_->store.emplace(hive::SettingsStore::instance());
    }

    // §Phase 16: initialize per-agent tool/MCP filter from AgentConfig.
    tool_filter_.disabled_tools = cfg_.tools_disabled;
    tool_filter_.disabled_mcp_servers = cfg_.mcp_servers_disabled;
}

void AgentImpl::set_state(AgentState s) noexcept
{
    AgentState old = state_.exchange(s, std::memory_order_acq_rel);
    if (old != s) {
        // §logging: state transitions at DEBUG — below the default INFO
        // level, so no log spam unless the level is lowered.
        LOG_DEBUG(std::string("Agent state: ") + hive::to_string(old)
                  + " -> " + hive::to_string(s));
    }
    if (old != s && sink_) {
        sink_->on_state_change(static_cast<int>(old), static_cast<int>(s));
        // §Phase 9.6: notify the host on turn completion. The CLI rings
        // the terminal bell, the webui plays a sound / flashes the
        // title — whatever the host's "attention please" gesture is.
        // Default no-op stays correct for hosts that don't care.
        if (s == AgentState::Done) {
            sink_->on_bell();
        }
    }
}

void AgentImpl::initialize()
{
    if (initialized_) return;

    // §logging: give the Logger a project-scoped home before anything else
    // logs. If it already self-initialized with the cwd fallback (a LOG_*
    // call ran before agent init), re-initialize with the per-agent project
    // directory. initialize() is hardened against fs errors; the try/catch
    // is belt-and-braces so logging can never break agent startup.
    {
        auto& logger = Logger::get_instance();
        if (!logger.is_initialized() || logger.is_auto_initialized()) {
            try {
                LoggerConfig lcfg;
                lcfg.log_path = "logs/";  // relative to the agent dir
                logger.initialize(
                    resolve_agent_dir(cfg_.project_root, cfg_.agent_name), lcfg);
            } catch (const std::exception& e) {
                if (sink_) {
                    sink_->on_log(hive::LogLevel::Warn,
                                  std::string("Logger init failed: ") + e.what());
                }
            }
        }
    }

    // §Phase 14.7: per-project system_prompt override.
    // Decision (see Agent.md §14.7 Q2): if
    // <project_root>/.daedalus/hive-brain/system_prompt.txt exists, its contents
    // are APPENDED to cfg_.system_prompt after the demo's default.
    // Demo default is the base; project file is concatenated after.
    // If cfg_.system_prompt is empty, only the project file is used.
    {
        std::error_code ec;
        fs::path override_path = resolve_agent_dir(cfg_.project_root, cfg_.agent_name)
                                 / "system_prompt.txt";
        if (fs::is_regular_file(override_path, ec)) {
            std::ifstream f(override_path);
            if (f) {
                std::ostringstream ss;
                ss << f.rdbuf();
                std::string project_text = ss.str();
                if (!project_text.empty()) {
                    if (cfg_.system_prompt.empty()) {
                        cfg_.system_prompt = std::move(project_text);
                    } else {
                        cfg_.system_prompt += "\n\n" + project_text;
                    }
                    if (sink_) sink_->on_log(hive::LogLevel::Info,
                        "[SYSTEM_PROMPT] Merged project override: " +
                        override_path.string());
                }
            }
        }
    }

    // §Phase 8.8: validate agent_name up-front so any later
    // Messages / SessionMemory init fails loudly with a clear
    // message rather than silently producing a bad path.
    if (!is_valid_agent_name(cfg_.agent_name)) {
        throw std::invalid_argument(
            "AgentConfig::agent_name is invalid: \"" + cfg_.agent_name +
            "\". Phase 8.8 rule: must match [A-Za-z0-9_.-]{1,64} and "
            "must not start with '.' or '-'. Empty string is allowed "
            "(means 'no namespace, use <project_root>/.daedalus/hive-brain/').");
    }

    // §Plan.md Phase 4 (2026-07-13): deprecation warning for legacy
    // ~/.daedalus/ user-tier layout. Per Plan.md §3.4, the library
    // honours the legacy path as a READ fallback for one release
    // cycle, but warns once per process so users notice.
    //
    // Detection rule (must match Paths::user_root() in src/library/paths.cpp):
    //   - $DAEDALUS_HOME unset (override wins, no deprecation)
    //   - $XDG_DATA_HOME unset  (XDG wins, no deprecation)
    //   - $HOME/.daedalus exists on disk → legacy layout in use
    //
    // When legacy layout is detected, emit one Warn via the sink so
    // the user sees it in their terminal. We do NOT print to stderr
    // (no-stdio rule, see Agent.md §Phase 5.1).
    {
        const char* home = std::getenv("HOME");
        const char* xdg_data = std::getenv("XDG_DATA_HOME");
        const char* daedalus_home = std::getenv("DAEDALUS_HOME");
        std::error_code ec;
        bool legacy_in_use = !daedalus_home && !xdg_data
                             && home && *home
                             && std::filesystem::is_directory(
                                 std::filesystem::path(home) / ".daedalus", ec);
        if (legacy_in_use && sink_) {
            sink_->on_log(hive::LogLevel::Warn,
                "[DEPRECATION] ~/.daedalus/ is the legacy user-tier layout. "
                "Per Plan.md §3.4 it is honoured as a READ fallback for one "
                "release cycle, then removed. Migrate via "
                "`daedalus-cli --migrate-data`, or set $DAEDALUS_HOME / "
                "$XDG_CONFIG_HOME / $XDG_DATA_HOME to opt into XDG-compliant "
                "locations immediately. See docs/storage.md (Plan.md Phase 6).");
        }
    }

    // §Plan.md §5.1: ensure the four XDG user-tier directories exist
    // with the right permissions BEFORE any subsystem writes to them.
    // This is the "auto-create on first write" half of Open Question #2
    // (the read side stays lazy — see Plan.md §5.2 Q2 for the rationale).
    //
    // Putting the call here (start of AgentImpl::initialize()) means:
    //   - Per-`Agent` instantiation, the dirs exist exactly once per
    //     process (because Paths::global() is the singleton).
    //   - Subsystems that write to mcp/.env / config.json / hive-brain.log
    //     downstream don't have to think about mkdir + chmod.
    //   - On legacy ~/.daedalus/ installs this also chmods the legacy
    //     dir to 0750 — a one-time tightening for users who haven't
    //     migrated yet.
    Paths::global().ensure_directories();

    // §Phase 16 (prompt.txt): OmniNet setup probe. OmniNet expects its
    // own config/data directory. If neither the XDG nor the legacy
    // ~/.omninet layout is present, warn the developer once via the
    // sink so they know network/WebSocket features need setup. We never
    // write to stderr here (no-stdio-in-library rule).
    {
        std::error_code ec;
        fs::path omninet_config = Paths::global().omninet_config_file();
        fs::path omninet_data   = Paths::global().omninet_data_dir();
        bool omninet_ready = fs::is_regular_file(omninet_config, ec)
                             || fs::is_directory(omninet_data, ec);
        if (!omninet_ready && sink_) {
            sink_->on_log(hive::LogLevel::Warn,
                "[OMNINET] OmniNet runtime config not found at "
                + omninet_config.string()
                + ". Some network/WebSocket features may be unavailable. "
                  "Run 'omninet-init' or create "
                + omninet_data.string()
                + " to set it up.");
        }
    }

    // §BUG-004 fix: honour cfg_.auto_approve even when no LLMClient is
    // constructed (e.g. hosts driving Agent::execute_tool() directly).
    // §BUG-007 fix: set the flag unconditionally — auto_approve()==false
    // must also take effect, otherwise a sticky global left ON by a previous
    // agent or a runtime toggle keeps bypassing the host's approval prompt.
    // AgentImpl::auto_approve() is the single authority for the effective
    // value (see also the LLMClient ctor, which honours AppConfig::auto_mode).
    ToolHandler::set_auto_mode(auto_approve());

    // §Phase 1+: only spin up the LLMClient if we have something to talk to.
    // "Explicit creds" now includes three sources (highest first):
    //   1. cfg_.model_name set                (e.g. --model minimax-m3)
    //   2. cfg_.api_key + cfg_.api_host set   (full override from CLI args)
    //   3. settings store has a default_model  (resolved below)
    //
    // Phase 8.5 (webui work): it was previously easy to ship the Agent
    // with no model at all if the host forgot to pass --model — the
    // settings default wasn't treated as a credential source.
    // The webui demo is exactly such a host: it knows nothing about
    // models and just passes cfg.daedalus_home. Now those hosts work
    // out of the box too.
    bool has_explicit_creds = !cfg_.model_name.empty()
        || (!cfg_.api_key.empty() && !cfg_.api_host.empty());
    // explicit_model_wins: the host owns the registry — never adopt the
    // settings store's default model behind its back.
    if (!cfg_.explicit_model_wins && !has_explicit_creds && body_->store) {
        try {
            auto default_model = body_->store->get_default_model_name();
            if (!default_model.empty()
                && body_->store->get_model(default_model).has_value()) {
                cfg_.model_name = default_model;
                has_explicit_creds = true;
            }
        } catch (...) { /* not fatal */ }
    }

    // §Phase 1+: honour cfg.clear_memory — wipe the project's session memory
    // AND the conversation-history file before loading them. Hosts can do this
    // once via the CLI flag; subsequent sessions start fresh.
    //
    // §Phase 8.5+: also wipe when --no-memory is set. The user-facing
    // semantics of "no memory" is "no prior context of any kind", which
    // includes the conversation history
    // (`<project>/.daedalus/hive-brain/messages.json`), not just the
    // SessionMemory notes. Without this, a long session (e.g. 1000+
    // messages) would exceed cheaper models' context windows (e.g.
    // MiniMax-M2.7 at 200K tokens) and the API would 400.
    if (cfg_.clear_memory || !cfg_.memory_enabled) {
        // Wipe the conversation-history file. SessionMemory::clear_all_memories
        // only covers the auxiliary notes; the message log is a separate file.
        try {
            // §Phase 8.8: target the per-agent namespace dir.
            fs::path messages_file = resolve_agent_dir(cfg_.project_root, cfg_.agent_name)
                                     / "messages.json";
            if (fs::exists(messages_file)) {
                fs::remove(messages_file);
                if (sink_) {
                    sink_->on_log(hive::LogLevel::Info,
                                  "[MEMORY] Conversation history wiped: " +
                                  messages_file.string());
                }
            }
        } catch (const std::exception& e) {
            if (sink_) {
                sink_->on_log(hive::LogLevel::Warn,
                              std::string("[MEMORY] Could not wipe messages.json: ") +
                              e.what());
            }
        }
    }

    if (cfg_.clear_memory) {
        // §Phase 8.8 / §Phase 16: clear memory lives under the
        // per-agent namespace so a named agent doesn't wipe its own peers.
        SessionMemory mem(resolve_agent_dir(cfg_.project_root, cfg_.agent_name));
        mem.clear_all_memories();
        if (sink_) {
            sink_->on_log(hive::LogLevel::Info,
                          "[MEMORY] All session memory cleared for this project");
        }
        // The flag is one-shot — clear it so re-initialize() doesn't repeat.
        cfg_.clear_memory = false;
    }

    // §FIX-MCP-STORE-LOAD (2026-07-17, plan R3-P1): MCP server
    // setup was inside `if (has_explicit_creds)` — meaning if the
    // user had MCP servers but no default model configured, MCP
    // setup was silently skipped entirely. MCP is independent of
    // LLM credentials: the user might configure MCP for testing
    // without an LLM, or set up MCP before picking a model.
    // The right gate is `cfg_.mcp_enabled`, not "has an LLM".
    if (cfg_.mcp_enabled) {
        body_->mcp = std::make_unique<mcp::McpServerManager>();
        // §Phase 15: install the per-server gate before load_from_store()
        // so disabled servers never enter configs_ in the first place.
        body_->mcp->set_disabled_servers(cfg_.mcp_servers_disabled);
        // Load servers from SettingsStore (SQLite).
        // load_env_file() populates env_vars_ from $XDG_CONFIG_HOME/hive-brain/mcp/.env
        body_->mcp->load_from_store(*body_->store);
        body_->mcp->load_env_file();
        if (auto factory = ToolHandler::get_tool_factory()) {
            factory->set_mcp_manager(body_->mcp.get());
        }
    }

    if (has_explicit_creds) {
        // Pass address of the contained SettingsStore (stable for Body lifetime).
        AppConfig app = make_app_config(cfg_, *body_->store);

        // §Phase 11: when mcp_enabled=false, body_->mcp is
        // nullptr and LLMClient's mcp_manager_ is null, so all MCP
        // operations become no-ops. (Setup happens in the
        // cfg_.mcp_enabled block above — moved out of this block
        // per §FIX-MCP-STORE-LOAD plan R3-P1 so MCP doesn't depend
        // on having LLM creds.)

        // Note: body_->skill_manager does not exist yet at this point
        // (it is created further below), so the constructor receives
        // nullptr here — the manager is attached via set_skill_manager()
        // right after it is created (§SKILL-ORDER fix).
        body_->client = std::make_unique<LLMClient>(
            app, std::addressof(*body_->store), body_->mcp.get(),
            body_->skill_manager.get(), &tool_filter_);
        if (sink_) {
            body_->client->set_sink(sink_);
        }
        // §State Machine: route LLM-driven transitions (set_status tool)
        // through AgentImpl::set_state so state_ stays the single source
        // of truth — Agent::state() is live mid-turn and on_state_change
        // always carries the correct prev.
        body_->client->set_state_change_callback(
            [this](hive::AgentState s) { set_state(s); });
        body_->client->initialize();
    }

    // §Phase 6: register any host tools with the ToolFactory.
    // ToolFactory::register_tool() is replace-by-name, so calling
    // initialize() multiple times (or after register_tool() before init)
    // doesn't double-register — it refreshes the adapter, which also
    // replaces stale adapters left by a previous Agent instance.
    if (auto factory = ToolHandler::get_tool_factory()) {
        for (auto& t : host_tools_) {
            factory->register_tool(
                std::make_shared<HostToolAdapter>(t, host_ctx_));
        }
    }

    // §Phase 9.7/§Phase 16: Skill loading driven by AgentConfig and owned
    // per-agent.  Previously the library used a process-wide singleton, but
    // runtime enable/disable must be scoped to each Agent.
    if (!cfg_.skills_dirs.empty() || !cfg_.skills.empty() ||
        !cfg_.skills_disabled.empty()) {
        try {
            body_->skill_manager = std::make_unique<SkillManager>();
            body_->skill_manager->init_after_settings();
            auto* sm = body_->skill_manager.get();

            // §Phase 15: install the per-skill gate BEFORE discovery
            // so the catalog loop in build_system_instruction() never
            // sees disabled skills, and activate_skill() refuses them
            // at runtime.
            sm->set_disabled_skills(cfg_.skills_disabled);
            // Add developer-specified directories to the search
            // list (append, don't replace — init_after_settings() already
            // added the default user/project directories).
            for (const auto& dir : cfg_.skills_dirs) {
                sm->add_skill_directory(dir);
            }
            // Discover everything currently visible. This populates
            // cached_skills_, so the discover_skills tool returns
            // the installed skills and the system prompt's Active
            // Skills count reflects reality.
            auto discovered = sm->discover_all_skills();
            // Activate any skills the host asked for by name. If a
            // name isn't in the cache (typo / not installed),
            // surface it via on_log instead of failing silently.
            size_t activated = 0;
            for (const auto& name : cfg_.skills) {
                auto loaded = sm->load_skill_by_name(name);
                if (loaded && sm->activate_skill(*loaded)) {
                    ++activated;
                } else {
                    if (sink_) sink_->on_log(hive::LogLevel::Warn,
                        "[SKILLS] Requested skill not found or not activatable: '"
                        + name + "' (not in any configured skills_dir)");
                }
            }
            if (sink_) {
                sink_->on_log(hive::LogLevel::Info,
                    "[SKILLS] Discovered " + std::to_string(discovered.size())
                    + " skill(s) from " + std::to_string(cfg_.skills_dirs.size())
                    + " dir(s); activated " + std::to_string(activated)
                    + " of " + std::to_string(cfg_.skills.size()) + " requested");
            }

            // §Phase 15: typo-detection for the disabled list.
            if (!cfg_.skills_disabled.empty() && sink_) {
                std::set<std::string> known;
                for (const auto& s : sm->get_all_cached_skills()) {
                    known.insert(s.first);
                }
                for (const auto& name : cfg_.skills_disabled) {
                    if (!known.count(name)) {
                        sink_->on_log(hive::LogLevel::Warn,
                            "[SKILLS] skills_disabled entry '" + name
                            + "' does not match any discovered skill (typo?)");
                    }
                }
            }
        } catch (const std::exception& e) {
            if (sink_) sink_->on_log(hive::LogLevel::Warn,
                std::string("[SKILLS] Discovery failed: ") + e.what());
        }
    }

    // §SKILL-ORDER fix: the LLMClient is constructed above (in the
    // has_explicit_creds block) BEFORE the SkillManager exists, so its
    // constructor always received nullptr for skill_manager and
    // LLMClient::skill_manager_ stayed null for the agent's lifetime.
    // That null propagated into SkillManagerGuard(skill_manager_) at the
    // top of every turn, making SkillManager::current() null — so the
    // discover_skills tool failed with "SkillManager not initialized"
    // and the system-prompt skills catalog was never emitted. Hand the
    // now-created manager to the client explicitly.
    if (body_->client && body_->skill_manager) {
        body_->client->set_skill_manager(body_->skill_manager.get());
    }

    // §Phase 15: typo-detection for the mcp_servers_disabled list.
    // Same idea as the skills one — warn if a disabled name doesn't
    // match any loaded server.
    //
    // §UB fix (2026-07-14): do NOT call list_configured_servers() twice
    // in the range-constructor expression. Each call returns a fresh
    // `vector<string>` by value, so `.begin()` and `.end()` end up
    // pointing into *different* containers — the resulting iterator
    // pair is ill-formed per [forward.iterators], and the set's range
    // constructor dereferences invalid memory. Symptom: SIGSEGV during
    // `_M_insert_unique_` (single-element inserts inside the unrolled
    // range loop). With creds resolved from the user's real config.json
    // this branch always runs; before, it was skipped because no
    // default model existed and `has_explicit_creds` was false.
    if (!cfg_.mcp_servers_disabled.empty() && body_->mcp && sink_) {
        const auto configured_servers = body_->mcp->list_configured_servers();
        std::set<std::string> known_servers(
            configured_servers.begin(), configured_servers.end());
        for (const auto& name : cfg_.mcp_servers_disabled) {
            if (!known_servers.count(name)) {
                sink_->on_log(hive::LogLevel::Warn,
                    "[MCP] mcp_servers_disabled entry '" + name
                    + "' does not match any loaded server (typo?)");
            }
        }
    }

    initialized_ = true;
    set_state(AgentState::Idle);
    if (sink_) {
        sink_->on_log(hive::LogLevel::Info,
                      "Agent::initialize() — ready"
                      + std::string(has_explicit_creds ? "" : " (no LLM client; no model configured)"));
    }
}

void AgentImpl::shutdown()
{
    if (!initialized_) return;
    // §CRASH-4 (2026-07-16): uninstall crash handlers BEFORE
    // tearing down LLMClient; otherwise the signal handler would
    // iterate a stale `messages_` pointer as Messages destructs.
    if (body_->client) {
        body_->client->uninstall_crash_handlers();
    }
    // §Phase 11: disconnect MCP servers before destroying the client.
    // Safe to call on nullptr (no-op).
    if (body_->mcp) {
        body_->mcp->disconnect_all();
    }
    if (body_->client) {
        body_->client->cleanup();
        body_->client.reset();
    }
    body_->mcp.reset();   // §Phase 11
    initialized_ = false;
    set_state(AgentState::Done);
    if (sink_) {
        sink_->on_log(hive::LogLevel::Info,
                      "Agent::shutdown() — done");
    }
}

// §CRASH-4 (2026-07-16): forward install/uninstall to the LLMClient
// (and from there to the underlying Messages instance).
void AgentImpl::install_crash_handlers() {
    if (!initialized_) {
        if (sink_) {
            sink_->on_log(hive::LogLevel::Warn,
                          "Agent::install_crash_handlers called before "
                          "initialize(); ignored. Call after agent.initialize().");
        }
        return;
    }
    if (body_->client) {
        body_->client->install_crash_handlers();
    }
}

void AgentImpl::uninstall_crash_handlers() {
    if (body_->client) {
        body_->client->uninstall_crash_handlers();
    }
}

void AgentImpl::submit_user_message(std::string text)
{
    pending_user_messages_.push_back({std::move(text), {}});
}

void AgentImpl::submit_user_message(std::string text,
                                    std::vector<ImageAttachment> images)
{
    // §READ-IMAGE-CORE follow-up (2026-08-10): attachments ride the queue
    // alongside the text and are handed to LLMClient::queue_image_attachments()
    // when the message drains — payload-only injection, vision-gated.
    pending_user_messages_.push_back({std::move(text), std::move(images)});
}

void AgentImpl::cancel() noexcept
{
    // §BUG-005 (2026-07-26): only latch cancel_requested_ when there is
    // work for it to interrupt — a turn in flight (busy state) or queued
    // user messages. Previously a bare cancel() at idle latched the flag,
    // and since it was only cleared by the exchange() at the entry of the
    // NEXT run_one_turn(), the following turn aborted instantly with a
    // spurious Cancelled (and a mid-flight cancel leaked the same way —
    // the interrupting turn never consumed the flag it had already
    // acted on via client->cancel()).
    const AgentState s = state_.load(std::memory_order_acquire);
    const bool busy = s == AgentState::Understanding    ||
                      s == AgentState::Planning         ||
                      s == AgentState::Acting           ||
                      s == AgentState::Verifying        ||
                      s == AgentState::Documenting      ||
                      s == AgentState::AwaitingApproval ||
                      s == AgentState::Compressing;
    if (busy || !pending_user_messages_.empty()) {
        cancel_requested_.store(true, std::memory_order_release);
    }
    if (body_->cancel_source.stop_possible()) {
        body_->cancel_source.request_stop();
    }
    // §Phase 5.1: also fire the LLMClient's cancel_source_ so the
    // SSE wait loop and per-iteration check exit promptly. Previously
    // the body_->cancel_source was a stranded signal — nothing in
    // the LLMClient read its token, so Agent::cancel() was effectively
    // a no-op for in-flight turns. Now the path is wired end-to-end.
    if (body_->client) {
        body_->client->cancel();
    }
}

void AgentImpl::run_one_turn(std::stop_token st)
{
    if (!initialized_) initialize();
    if (cancel_requested_.exchange(false)) {
        set_state(AgentState::Cancelled);
        return;
    }
    if (pending_user_messages_.empty()) {
        // No queued message — no-op.
        return;
    }
    if (st.stop_requested()) {
        set_state(AgentState::Cancelled);
        return;
    }

    LOG_DEBUG("run_one_turn: dispatching "
              + std::to_string(pending_user_messages_.size())
              + " pending message(s)");

    if (!body_->client) {
        // No LLMClient spun up (no model credentials configured). Surface
        // as Error rather than crashing. File-log unconditionally (also
        // covers the no-sink case, which was previously dropped silently).
        std::string msg = std::move(pending_user_messages_.front().text);
        pending_user_messages_.erase(pending_user_messages_.begin());
        LOG_ERROR("[Internal] Agent::run_one_turn() — no model configured; "
                  "cannot dispatch message: " + msg);
        if (sink_) {
            sink_->on_error(hive::ErrorKind::Internal,
                            "Agent::run_one_turn() — no model configured; "
                            "cannot dispatch message: " + msg,
                            /*will_retry=*/false);
        }
        set_state(AgentState::Error);
        return;
    }

    PendingUserMessage pending = std::move(pending_user_messages_.front());
    pending_user_messages_.erase(pending_user_messages_.begin());

    set_state(AgentState::Understanding);
    if (!pending.images.empty()) {
        body_->client->queue_image_attachments(pending.images);
    }
    ::AgentState r = body_->client->run_one_turn(pending.text);
    // §BUG-005: a cancel() that interrupted this turn has been fully
    // consumed — never let the flag leak into the next turn.
    cancel_requested_.store(false, std::memory_order_release);
    set_state(map_from_internal(r));
}

AgentStats AgentImpl::stats() const noexcept
{
    AgentStats s = stats_;
    if (body_->client) {
        s.iteration         = body_->client->get_iteration_count();
        s.max_iterations    = body_->client->get_max_iterations();
        s.prompt_tokens     = body_->client->get_prompt_tokens();
        s.completion_tokens = body_->client->get_completion_tokens();
        s.cached_tokens     = body_->client->get_cached_tokens();
        s.total_tokens      = body_->client->get_total_tokens();
        s.max_context_size  = body_->client->get_max_context_size();
        s.message_count     = body_->client->get_message_count();
    }
    return s;
}

std::vector<std::string> AgentImpl::mcp_configured_servers() const
{
    // §FIX-MCP-STORE-LOAD (2026-07-17): thin forwarder. Previously
    // AgentImpl built an McpServerManager but never loaded it from the
    // store, so this would have returned []. Now returns whatever was
    // Returns the MCP servers loaded from SettingsStore by AgentImpl::AgentImpl.
    // Empty when mcp_enabled=false (body_->mcp is null).
    std::vector<std::string> out;
    if (!body_ || !body_->mcp) return out;
    return body_->mcp->list_configured_servers();
}

std::vector<ToolInfo> AgentImpl::list_tools() const
{
    // Surface ToolFactory::get_all_definitions() as ToolInfo records.
    // §Phase 16: apply the per-agent disabled-tool filter.
    std::vector<ToolInfo> out;
    auto factory = ToolHandler::get_tool_factory();
    if (!factory) return out;
    json defs = factory->get_all_definitions(tool_filter_);
    if (!defs.is_array()) return out;
    out.reserve(defs.size());
    for (const auto& def : defs) {
        ToolInfo ti;
        if (def.contains("function")) {
            const auto& fn = def["function"];
            ti.name        = fn.value("name", "");
            ti.description = fn.value("description", "");
            if (fn.contains("parameters")) ti.schema = fn["parameters"];
        } else {
            ti.name        = def.value("name", "");
            ti.description = def.value("description", "");
            if (def.contains("parameters")) ti.schema = def["parameters"];
        }
        if (!ti.name.empty()) out.push_back(std::move(ti));
    }
    return out;
}

std::vector<SkillInfo> AgentImpl::list_skills() const
{
    // §Phase 16: surface this Agent's owned SkillManager. Runtime disabled
    // skills are hidden via the visible view.
    std::vector<SkillInfo> out;
    SkillManager* sm = body_->skill_manager ? body_->skill_manager.get()
                                            : SkillManager::current();
    if (!sm) return out;
    auto skills = sm->get_visible_cached_skills();
    out.reserve(skills.size());
    for (const auto& [name, s] : skills) {
        if (!s.is_valid()) continue;
        SkillInfo si;
        si.name        = s.metadata.name;
        si.description = s.metadata.description;
        si.source_path = s.path.string();
        out.push_back(std::move(si));
    }
    return out;
}

bool AgentImpl::client_has_skill_manager() const noexcept
{
    return body_ && body_->client && body_->client->has_skill_manager();
}

// §B18 refactor: shared helper to build ModelInfo from resolved settings.
// Both list_models() and active_model() use the same merge of provider +
// model fields; keeping the conversion in one place prevents divergence.
static ModelInfo to_model_info(const hive::ResolvedModelConfig& cfg,
                               const hive::SettingsStore& store) {
    ModelInfo mi;
    mi.name            = cfg.name;
    mi.provider        = cfg.provider;
    mi.model_id        = cfg.model_id;
    mi.host            = cfg.host;
    mi.chat_endpoint   = cfg.chat_endpoint;
    mi.max_context_size = (cfg.max_context_size == 0)
                              ? store.get_max_context_size_for_model(cfg.name)
                              : cfg.max_context_size;
    mi.temperature = cfg.temperature;  // §Phase 8.7
    return mi;
}

static ModelInfo to_model_info(const hive::ModelConfig& cfg,
                               const hive::SettingsStore& store) {
    if (auto resolved = store.resolve_model(cfg.name)) {
        return to_model_info(*resolved, store);
    }
    // Fallback for models with missing provider — should not happen after v4.
    ModelInfo mi;
    mi.name     = cfg.name;
    mi.provider = cfg.provider;
    mi.model_id = cfg.model_id;
    mi.max_context_size = store.get_max_context_size_for_model(cfg.name);
    mi.temperature = cfg.temperature;
    return mi;
}

std::vector<ModelInfo> AgentImpl::list_models() const
{
    std::vector<ModelInfo> out;
    if (!body_->store) return out;
    const auto models = body_->store->list_models();
    out.reserve(models.size());
    for (const auto& cfg : models) {
        out.push_back(to_model_info(cfg, *body_->store));
    }
    return out;
}

std::vector<hive::ThinkingLevel> AgentImpl::supported_thinking_levels() const
{
    if (!body_->client) return {};
    return body_->client->supported_thinking_levels();
}

void AgentImpl::set_thinking_level(hive::ThinkingLevel level) noexcept
{
    // §ThinkingLevel: runtime toggle. Store in cfg_ so rebuilds/model
    // switches keep the selection, and forward to the live LLMClient —
    // the payload hook reads config_.thinking_level per request, so the
    // very next turn uses the new level.
    cfg_.thinking_level = level;
    if (body_ && body_->client) body_->client->set_thinking_level(level);
}

void AgentImpl::compress_context() noexcept
{
    // §AutoCompress: host-facing trigger. The LLMClient compresses
    // immediately when no turn is running; otherwise the request is
    // consumed at the end-of-cycle checkpoint (also on the cancelled-turn
    // path) — same code path as the automatic 90%-threshold trigger.
    if (body_ && body_->client) body_->client->compress_context();
}

ModelInfo AgentImpl::active_model() const noexcept
{
    // §Phase 8.5+: report the model that's actually loaded for this
    // session. Resolution priority (highest first):
    //   1. cfg.model_name (explicit --model argument)
    //   2. settings store default_model
    //   3. first model in settings store
    if (!body_->store) return {};
    const auto& store = *body_->store;
    const auto models = store.list_models();

    // §B18: uses shared to_model_info() helper.
    auto find_by_name = [&](const std::string& name) -> std::optional<ModelInfo> {
        for (const auto& cfg : models) {
            if (cfg.name == name) return to_model_info(cfg, store);
        }
        return std::nullopt;
    };

    // 1. cfg.model_name
    if (!cfg_.model_name.empty()) {
        if (auto m = find_by_name(cfg_.model_name)) return *m;
    }
    // explicit_model_wins: the host owns the registry — never fall through
    // to settings-store models; report the explicit config instead.
    if (cfg_.explicit_model_wins) {
        if (cfg_.model_name.empty()) return {};
        ModelInfo mi;
        mi.name            = cfg_.model_name;  // hosts inject the model id here
        mi.model_id        = cfg_.model_name;
        mi.host            = cfg_.api_host;
        mi.chat_endpoint   = cfg_.chat_endpoint.empty()
                                 ? "/v1/chat/completions" : cfg_.chat_endpoint;
        mi.max_context_size = cfg_.max_context_size;
        mi.temperature     = cfg_.temperature;
        return mi;
    }
    // 2. default_model from settings store
    try {
        auto default_name = store.get_default_model_name();
        if (!default_name.empty()) {
            if (auto m = find_by_name(default_name)) return *m;
        }
    } catch (...) { /* not fatal */ }
    // 3. first model in settings store
    if (!models.empty()) {
        return to_model_info(models.front(), store);
    }
    return {};
}

std::string AgentImpl::library_version() noexcept
{
#ifdef DAEDALUS_GIT_VERSION
    return DAEDALUS_GIT_VERSION;
#else
    return "v0.0.0";
#endif
}

ModelDiscoveryResult AgentImpl::discover_models(
    const std::string& model_name,
    const std::filesystem::path& daedalus_home)
{
    std::optional<ResolvedModelConfig> cfg;
    if (daedalus_home.empty()) {
        cfg = SettingsStore::instance().resolve_model(model_name);
    } else {
        cfg = SettingsStore::for_home(daedalus_home).resolve_model(model_name);
    }

    if (!cfg) {
        ModelDiscoveryResult result;
        result.error_message =
            "Model discovery failed: model '" + model_name + "' not found";
        return result;
    }

    return hive::discover_models(*cfg);
}

bool AgentImpl::auto_approve() const noexcept
{
    // §BUG-006: when the host sets cfg.auto_approve it is authoritative —
    // the SettingsStore row is ignored so a host can force approval prompts
    // ON even if another front-end persisted auto_approve=true. Unset =
    // legacy behaviour: fall back to the store.
    if (cfg_.auto_approve) return *cfg_.auto_approve;
    if (body_->store) {
        try {
            return body_->store->get_auto_approve();
        } catch (...) { /* fall through */ }
    }
    return false;
}

void AgentImpl::set_auto_approve(bool enabled) noexcept
{
    // §BUG-006/007: the host's runtime toggle is authoritative. Store it in
    // cfg_ so initialize()/execute_tool() re-apply it, and flip the global
    // ToolHandler flag immediately so a mid-session toggle takes effect on
    // the very next command.
    cfg_.auto_approve = enabled;
    ToolHandler::set_auto_mode(enabled);
}

void AgentImpl::clear_session_approvals() noexcept
{
    // §BUG-008: drop the in-memory approved-command cache.
    ToolHandler::clear_session_approved_commands();
}

void AgentImpl::clear_persisted_approvals() noexcept
{
    // §BUG-008: delete this project's approvals.json and clear the
    // in-memory persisted set.
    try {
        ToolHandler::clear_persisted_approvals(
            cfg_.project_root.empty() ? std::filesystem::current_path()
                                      : cfg_.project_root,
            cfg_.agent_name);
    } catch (...) { /* noexcept contract */ }
}

void AgentImpl::set_remember_approvals(bool enabled) noexcept
{
    // §BUG-008: opt out of auto-recording host-approved commands.
    ToolHandler::set_remember_approvals(enabled);
}

nlohmann::json AgentImpl::execute_tool(const std::string& name,
                                       const nlohmann::json& args)
{
    if (!initialized_) initialize();

    // §B17 fix: route through ToolHandler so the public API actually works.
    // Synthesise an OpenAI-style tool_call so ToolHandler understands it.
    static std::atomic<int> next_id{0};
    std::string call_id = "host_" + std::to_string(next_id.fetch_add(1));

    nlohmann::json tool_call = {
        {"id",     call_id},
        {"type",   "function"},
        {"function", {
            {"name",      name},
            {"arguments", args.is_string() ? args.get<std::string>()
                                          : args.dump()}
        }}
    };

    // §BUG-003 fix: FileIO must be rooted at the configured project
    // root, not the process CWD — hosts routinely run the agent from
    // a different directory than the project.
    FileIO file_io(cfg_.project_root.empty()
                       ? std::filesystem::current_path()
                       : cfg_.project_root,
                   cfg_.agent_name);

    // §BUG-004 fix: honour cfg_.auto_approve even when no LLMClient
    // was ever constructed (the LLMClient ctor is otherwise the only
    // place that flips ToolHandler's auto mode).
    // §BUG-007 fix: set unconditionally so auto_approve()==false also
    // takes effect (a sticky global must not keep bypassing approvals).
    ToolHandler::set_auto_mode(auto_approve());

    // §Phase 16: route skill tools to this Agent's owned SkillManager and
    // enforce the per-agent tool filter for direct execution.
    struct SkillManagerGuard {
        SkillManager* prev_;
        explicit SkillManagerGuard(SkillManager* sm)
            : prev_(SkillManager::current()) {
            SkillManager::set_current(sm);
        }
        ~SkillManagerGuard() { SkillManager::set_current(prev_); }
    } skill_guard(body_->skill_manager.get());

    nlohmann::json result = ToolHandler::execute_tool_call(
        tool_call, file_io, nullptr, tool_filter_);

    if (sink_) {
        sink_->on_log(hive::LogLevel::Info,
                      "Agent::execute_tool(" + name + ") → "
                      + result.value("status", "unknown"));
    }

    return result;
}

// ============================================================================
// Todo list public API
// ============================================================================

static fs::path todo_project_root(const fs::path& cfg_root)
{
    fs::path root = cfg_root.empty() ? fs::current_path() : cfg_root;
    std::error_code ec;
    if (fs::exists(root, ec)) {
        return fs::canonical(root, ec);
    }
    return fs::absolute(root);
}

nlohmann::json AgentImpl::add_todo(const std::string& text,
                                   const std::optional<std::string>& status,
                                   const std::optional<std::string>& group)
{
    return TodoListService::instance()
        .get(todo_project_root(cfg_.project_root), cfg_.agent_name)
        .add(text, status, group);
}

nlohmann::json AgentImpl::edit_todo(int id,
                                    const std::optional<std::string>& text,
                                    const std::optional<std::string>& status,
                                    const std::optional<std::string>& group)
{
    return TodoListService::instance()
        .get(todo_project_root(cfg_.project_root), cfg_.agent_name)
        .edit(id, text, status, group);
}

nlohmann::json AgentImpl::delete_todo(int id)
{
    return TodoListService::instance()
        .get(todo_project_root(cfg_.project_root), cfg_.agent_name)
        .remove(id);
}

nlohmann::json AgentImpl::list_todos(const std::string& filter,
                                     const std::optional<std::string>& group)
{
    return TodoListService::instance()
        .get(todo_project_root(cfg_.project_root), cfg_.agent_name)
        .list(filter, group);
}

nlohmann::json AgentImpl::count_todos()
{
    return TodoListService::instance()
        .get(todo_project_root(cfg_.project_root), cfg_.agent_name)
        .count();
}

nlohmann::json AgentImpl::set_max_todos(int max)
{
    return TodoListService::instance()
        .get(todo_project_root(cfg_.project_root), cfg_.agent_name)
        .set_max(max);
}

nlohmann::json AgentImpl::clear_todos()
{
    return TodoListService::instance()
        .get(todo_project_root(cfg_.project_root), cfg_.agent_name)
        .clear();
}

void AgentImpl::register_tool(std::unique_ptr<BaseTool> tool)
{
    if (!tool) return;

    // 1. Take ownership in our own vector (so the tool survives).
    std::shared_ptr<BaseTool> shared = std::move(tool);
    host_tools_.push_back(shared);

    // 2. Wrap in HostToolAdapter and register with the ToolFactory so
    //    the LLMClient's tool-dispatch path finds it.
    if (body_->client) {
        auto factory = ToolHandler::get_tool_factory();
        if (factory) {
            auto adapter = std::make_shared<HostToolAdapter>(shared, host_ctx_);
            factory->register_tool(adapter);
            if (sink_) {
                sink_->on_log(hive::LogLevel::Info,
                              "[TOOL] Registered custom tool: " + shared->get_name());
            }
        }
    } else if (sink_) {
        sink_->on_log(hive::LogLevel::Warn,
                      "[TOOL] Agent not yet initialized; tool '"
                      + shared->get_name()
                      + "' will be registered on next initialize()");
    }
}

void AgentImpl::clear_memory()
{
    // §Phase 8.8 / §Phase 16: per-agent namespace (see initialize()'s
    // same call).
    SessionMemory mem(resolve_agent_dir(cfg_.project_root, cfg_.agent_name));
    mem.clear_all_memories();
    if (sink_) {
        sink_->on_log(hive::LogLevel::Info,
                      "[MEMORY] All session memory cleared for this project");
    }
}

bool AgentImpl::switch_model(const std::string& model_name)
{
    if (!body_->store || model_name.empty()) return false;

    auto opt = body_->store->get_model(model_name);
    if (!opt.has_value()) {
        // model_name may be a provider model id (e.g. discovered via /v1/models).
        // If a current provider is set, create a new ModelConfig using the
        // provider's host/api_key so the user can switch to discovered models.
        const auto provider_name = body_->store->get_current_provider();
        if (provider_name.empty()) return false;

        auto provider = body_->store->get_provider(provider_name);
        if (!provider.has_value()) return false;

        ModelConfig discovered;
        discovered.name      = model_name;
        discovered.provider  = provider_name;
        discovered.model_id  = model_name;
        body_->store->upsert_model(discovered);
        opt = discovered;
    }

    body_->store->set_default_model_name(model_name);
    cfg_.model_name = model_name;

    // §MODEL-SWITCH: the new model may have a smaller context window,
    // so clear the conversation history and reconfigure the LLMClient
    // to use the new model's endpoint / credentials / context size.
    if (body_->client) {
        AppConfig new_app = make_app_config(cfg_, *body_->store);
        body_->client->set_config(new_app);
        body_->client->clear_messages();
    } else if (initialized_) {
        // §MODEL-SWITCH-CREATE: no LLMClient was created at startup (no
        // resolvable default model), so there is nothing to reconfigure —
        // create the client now that a model has been selected, otherwise
        // run_one_turn() reports "no model configured" forever.
        AppConfig app = make_app_config(cfg_, *body_->store);
        body_->client = std::make_unique<LLMClient>(
            app, std::addressof(*body_->store), body_->mcp.get(),
            body_->skill_manager.get(), &tool_filter_);
        if (sink_) {
            body_->client->set_sink(sink_);
        }
        body_->client->set_state_change_callback(
            [this](hive::AgentState s) { set_state(s); });
        body_->client->initialize();
    }

    if (sink_) {
        sink_->on_log(hive::LogLevel::Info,
                      "[MODEL] Switched to " + model_name);
    }
    return true;
}

std::vector<std::string> AgentImpl::list_providers() const
{
    if (!body_->store) return {};
    return body_->store->list_providers();
}

std::vector<ProviderConfig> AgentImpl::list_providers_detailed() const
{
    if (!body_->store) return {};
    return body_->store->list_providers_detailed();
}

bool AgentImpl::upsert_provider(const ProviderConfig& provider)
{
    if (!body_->store || provider.name.empty() || provider.host.empty()) return false;
    body_->store->upsert_provider(provider);
    return true;
}

bool AgentImpl::delete_provider(const std::string& provider)
{
    if (!body_->store || provider.empty()) return false;
    body_->store->delete_provider(provider);
    return true;
}

bool AgentImpl::upsert_model(const ModelConfig& model)
{
    if (!body_->store || model.name.empty() || model.provider.empty() || model.model_id.empty()) return false;
    if (!body_->store->get_provider(model.provider).has_value()) return false;
    body_->store->upsert_model(model);
    return true;
}

bool AgentImpl::delete_model(const std::string& name)
{
    if (!body_->store || name.empty()) return false;
    body_->store->delete_model(name);
    return true;
}

bool AgentImpl::switch_provider(const std::string& provider)
{
    if (!body_->store || provider.empty()) return false;

    // Verify the provider exists and has at least one model.
    auto provider_cfg = body_->store->get_provider(provider);
    if (!provider_cfg.has_value()) return false;

    std::optional<ModelConfig> provider_model;
    for (const auto& m : body_->store->list_models()) {
        if (m.provider == provider) {
            provider_model = m;
            break;
        }
    }
    if (!provider_model.has_value()) return false;

    body_->store->set_current_provider(provider);

    // If the current model is not part of this provider, switch to the first
    // model of the new provider so subsequent turns actually use it.
    bool current_belongs = false;
    if (!cfg_.model_name.empty()) {
        if (auto m = body_->store->get_model(cfg_.model_name);
            m.has_value() && m->provider == provider) {
            current_belongs = true;
        }
    }
    if (!current_belongs) {
        switch_model(provider_model->name);
    }

    if (sink_) {
        sink_->on_log(hive::LogLevel::Info,
                      "[PROVIDER] Switched to " + provider);
    }
    return true;
}

ModelDiscoveryResult AgentImpl::discover_provider_models() const
{
    ModelDiscoveryResult result;
    if (!body_->store) {
        result.error_message = "No settings store available";
        return result;
    }

    const auto provider_name = body_->store->get_current_provider();
    if (provider_name.empty()) {
        result.error_message = "No current provider set; use /provider <name> first";
        return result;
    }

    auto provider = body_->store->get_provider(provider_name);
    if (!provider.has_value()) {
        result.error_message = "Provider not configured: " + provider_name;
        return result;
    }

    // §ProviderPlugins: rows with an empty discovery_endpoint (written
    // before endpoint defaults were enforced) inherit the plugin's
    // models endpoint.
    std::string discovery = provider->discovery_endpoint;
    if (discovery.empty()) {
        std::shared_ptr<hive::ProviderPlugin> plugin;
        if (!provider->plugin.empty()) {
            plugin = hive::ProviderRegistry::plugin_for_name(provider->plugin);
        }
        if (!plugin) {
            plugin = hive::ProviderRegistry::resolve(provider->name, provider->host);
        }
        discovery = plugin->models_endpoint();
    }

    return hive::discover_models(
        provider->host, provider->api_key, discovery);
}

std::string AgentImpl::last_assistant_text() const
{
    if (!body_->client) return {};
    return body_->client->get_last_assistant_text();
}

// ============================================================================
// Runtime enable/disable for skills, MCP servers, and tools
// ============================================================================

bool AgentImpl::set_skill_enabled(const std::string& name, bool enabled)
{
    if (!body_->skill_manager) {
        // No skill manager means no skills were configured; nothing to toggle.
        return false;
    }

    auto& sm = *body_->skill_manager;

    // Reject unknown skill names so hosts cannot silently "disable" a typo.
    if (!sm.get_all_cached_skills().count(name)) {
        if (sink_) {
            sink_->on_log(hive::LogLevel::Warn,
                "[SKILLS] Cannot " + std::string(enabled ? "enable" : "disable") +
                " unknown skill '" + name + "'");
        }
        return false;
    }

    auto disabled = sm.get_disabled_skills();
    const bool was_disabled = disabled.count(name);

    if (enabled) {
        if (!was_disabled) return false;
        disabled.erase(name);
    } else {
        if (was_disabled) return false;
        disabled.insert(name);
        // If the skill is currently active, deactivate it so its context
        // disappears immediately.
        sm.deactivate_skill(name);
    }

    sm.set_disabled_skills(disabled);

    if (sink_) {
        sink_->on_log(hive::LogLevel::Info,
            "[SKILLS] " + name + " " + std::string(enabled ? "enabled" : "disabled")
            + " at runtime");
    }
    return true;
}

bool AgentImpl::is_skill_enabled(const std::string& name) const
{
    if (!body_->skill_manager) return false;
    return !body_->skill_manager->is_skill_disabled(name);
}

std::vector<std::string> AgentImpl::disabled_skills() const
{
    if (!body_->skill_manager) return {};
    const auto& disabled = body_->skill_manager->get_disabled_skills();
    return std::vector<std::string>(disabled.begin(), disabled.end());
}

bool AgentImpl::set_mcp_server_enabled(const std::string& name, bool enabled)
{
    if (!body_->mcp || !body_->store) return false;

    auto& mgr = *body_->mcp;
    const bool currently_disabled = mgr.is_server_disabled(name);

    if (enabled) {
        if (!currently_disabled) return false;
        // Load the server config from the store so we can reconnect.
        auto server = body_->store->get_mcp_server(name);
        if (!server.has_value()) return false;

        mcp::McpServerConfig sc;
        sc.name         = server->name;
        sc.command      = server->command;
        sc.args         = server->args;
        sc.env          = server->env;
        sc.transport    = server->transport;
        sc.url          = server->url;
        sc.bearer_token = server->bearer_token;
        sc.description  = server->description;
        sc.enabled      = server->enabled;

        if (!sc.is_valid()) return false;

        bool ok = mgr.enable_server(sc);
        tool_filter_.disabled_mcp_servers.erase(name);
        if (sink_) {
            sink_->on_log(hive::LogLevel::Info,
                "[MCP] Server '" + name + "' enabled at runtime");
        }
        return ok;
    }

    if (currently_disabled) return false;

    // Reject unknown server names so hosts cannot silently "disable" a typo.
    if (!body_->store || !body_->store->get_mcp_server(name).has_value()) {
        if (sink_) {
            sink_->on_log(hive::LogLevel::Warn,
                "[MCP] Cannot disable unknown server '" + name + "'");
        }
        return false;
    }

    mgr.disable_server(name);
    tool_filter_.disabled_mcp_servers.insert(name);
    if (sink_) {
        sink_->on_log(hive::LogLevel::Info,
            "[MCP] Server '" + name + "' disabled at runtime");
    }
    return true;
}

bool AgentImpl::is_mcp_server_enabled(const std::string& name) const
{
    if (!body_->mcp) return false;
    return !body_->mcp->is_server_disabled(name);
}

std::vector<std::string> AgentImpl::disabled_mcp_servers() const
{
    if (!body_->mcp) return {};
    const auto disabled = body_->mcp->get_disabled_servers();
    return std::vector<std::string>(disabled.begin(), disabled.end());
}

// §ProviderPlugins
std::string AgentImpl::active_provider_plugin() const
{
    if (!body_ || !body_->client) return {};
    return body_->client->provider_plugin_name();
}

bool AgentImpl::set_tool_enabled(const std::string& name, bool enabled)
{
    auto factory = ToolHandler::get_tool_factory();
    if (!factory || !factory->has_tool(name)) {
        if (sink_) {
            sink_->on_log(hive::LogLevel::Warn,
                "[TOOLS] Cannot " + std::string(enabled ? "enable" : "disable") +
                " unknown tool '" + name + "'");
        }
        return false;
    }

    const bool currently_disabled = tool_filter_.disabled_tools.count(name);
    if (enabled) {
        if (!currently_disabled) return false;
        tool_filter_.disabled_tools.erase(name);
    } else {
        if (currently_disabled) return false;
        tool_filter_.disabled_tools.insert(name);
    }

    if (sink_) {
        sink_->on_log(hive::LogLevel::Info,
            "[TOOLS] " + name + " " + std::string(enabled ? "enabled" : "disabled")
            + " at runtime");
    }
    return true;
}

bool AgentImpl::is_tool_enabled(const std::string& name) const
{
    return tool_filter_.is_tool_allowed(name);
}

std::vector<std::string> AgentImpl::disabled_tools() const
{
    return std::vector<std::string>(
        tool_filter_.disabled_tools.begin(),
        tool_filter_.disabled_tools.end());
}

}  // namespace hive::detail