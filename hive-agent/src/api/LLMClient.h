#pragma once

#include <deque>
#include <string>
#include <nlohmann/json.hpp>
#include "types/ImageAttachment.h"
#include <filesystem>
#include <functional>
#include <memory>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <stop_token>

#include "api/HttpSession.h"
#include "core/FileIO.h"
#include "core/SessionMemory.h"
#include "core/Messages.h"
#include "mcp/McpClient.h"
// §Phase 11: McpServerManager* is now passed in via the ctor (injected),
// not accessed via the singleton. Forward-declare instead of including.
namespace mcp { class McpServerManager; }
class SkillManager;                        // §Phase 16: per-agent skill manager
#include "tools/ToolHandler.h"
#include "tools/ToolFilter.h"

#include "library/AgentSink.h"
#include "api/ProviderPlugin.h"   // §ProviderPlugins
#include "core/settings.h"   // §v0.5.0: SettingsStore* replaces SettingsManager*
#include "types/AgentState.h"  // §State Machine: public state for the change callback

using json = nlohmann::json;

struct AppConfig {
    std::string base_url;
    std::string api_key;
    std::string model;            // model_id (sent to API), e.g. "MiniMax-M2.7"
    std::string model_name;       // settings.json key, e.g. "minimax-m2.7"
    std::string provider;         // §ProviderPlugins: provider label (plugin selection)
    std::string plugin;           // §ProviderPlugins: explicit plugin name from the
                                  // providers row; empty = auto-detect
    std::string chat_endpoint;
    std::filesystem::path target_directory;
    std::string agent_name;        // §Phase 8.8 — per-agent runtime-state namespace
    // §ContextCache: stable session id (from AgentConfig::session_id).
    // Fallback for the server-side prompt-cache key (e.g. Moonshot's
    // prompt_cache_key, which Kimi Code Plan requires for good hit
    // rates) when agent_name is empty (the default agent).
    std::string session_id;
    bool auto_mode = false;  // Automatically approve tool executions
    bool memory_disabled = false;  // Disable session memory
    // §ContextCache follow-up (2026-08-02): omit the Session Memory
    // section from the composed system prompt (memory collection is
    // unaffected). Mapped from AgentConfig::memory_in_prompt.
    bool memory_in_prompt = true;
    // §Phase 9.8: SSE streaming. Default = true (progressive tokens).
    // Set to false to get a single buffered JSON response instead.
    bool stream = true;

    // §Phase 8.7: resolved sampling temperature.
    // Priority (highest first):
    //   1. cfg.temperature >= 0  (explicit session override)
    //   2. model's temperature from settings.json
    //   3. 0.0 (deterministic default)
    double temperature = 0.0;

    // Host-selected thinking level (2026-08-01). Passed through to the
    // provider plugin via ProviderChatContext; Default = send nothing.
    hive::ThinkingLevel thinking_level = hive::ThinkingLevel::Default;

    // §Phase 6: host-supplied role prompt (from AgentConfig::system_prompt).
    // The library prepends it to the composed system prompt so the host
    // can fully control the agent's persona. Empty = library default role.
    std::string system_prompt;

    // §Phase 14: optional 1-line note about the system_info tool. The host
    // can use this to remind the agent when to call system_info (or not).
    // Empty = no note appended.
    std::string system_info_note;

    // §UA: HTTP User-Agent sent on LLM API requests (both the streaming
    // path via HttpSession and the sync OmniNet fallback). Defaults to the
    // library identity; hosts override it via AgentConfig::user_agent.
    std::string user_agent = "hive-brain/1.0";

    // Phase 2.7: removed sub_agent_mode, sub_agent_looping, manager_mode.
    // The Agent has no notion of modes — those are host concerns. Demos
    // for manager / sub-agent patterns live in demos/daedalus-manager/
    // and demos/daedalus-subagent/.
};

// Agent state machine states
enum class AgentState : std::uint8_t {
    UNDERSTAND,  // Analyse the project and focus on files relevant to the user's prompt
    PLANNING,    // Research with grep_search, analyze task, and formulate strategy
    ACT,         // Execution phase - implements changes
    BUILD_TEST,  // Verification phase - runs compiler/tests
    DONE,        // Task completed successfully
    CANCELLED    // Interrupted by user (ESC key)
};

// §State Machine: callback used to report LLM-driven public-state
// transitions (set_status tool) up to the owner (AgentImpl).
using StateChangeCallback = std::function<void(hive::AgentState)>;

// §Phase 5.1: ESCKeyDetector removed. Library has no terminal
// coupling — no termios, no stdin polling. Cancellation flows
// through `Agent::cancel()` → `LLMClient::cancel()` → a
// std::stop_source that's checked in the API wait loop. Hosts
// that want ESC-to-cancel capture it themselves (e.g. by reading
// from stdin in a separate thread, then calling agent.cancel()).

class LLMClient {
private:
    AppConfig config_;
    // §ProviderPlugins: resolved from config_.provider/base_url via
    // ProviderRegistry in the ctor and set_config(). Never null after
    // resolution — falls back to the OpenAI-compatible default plugin.
    std::shared_ptr<hive::ProviderPlugin> plugin_;
    std::shared_ptr<hive::AgentSink> sink_;  // §Phase 3: routes all I/O + streaming
    std::unique_ptr<FileIO> file_io_;
    hive::SettingsStore* settings_store_ = nullptr;  // §v0.5.0: wired directly to SettingsStore
    std::unique_ptr<SessionMemory> session_memory_;
    std::unique_ptr<Messages> messages_;
    int iteration_count_;
    // §LoopGuard: sliding window of recent tool-call signatures
    // (name + args) plus trip counter for the degenerate-loop breaker.
    std::deque<std::string> loop_guard_window_;
    int loop_guard_trips_ = 0;
    int max_iterations_ = 100;
    size_t max_tool_result_size_ = 10000;  // 10KB - truncate middle for tool results
    size_t max_request_size_ = 4000000;   // 4MB default
    size_t max_response_size_ = 5000000;  // 5MB default
    int last_total_tokens_ = 0;
    int last_prompt_tokens_ = 0;
    int last_completion_tokens_ = 0;
    // §ContextCache: prompt tokens served from the provider's server-side
    // context cache on the last response (0 = miss / unsupported).
    int last_cached_tokens_ = 0;

    // §B32: suppresses duplicate "no usage" warnings per turn.
    bool warned_no_usage_this_turn_ = false;

    // §AutoCompress: host-requested compression flag. Set via
    // compress_context() (any thread) when a turn is running, consumed at
    // the compression checkpoint at the end of an agent cycle (and on the
    // cancelled-turn path).
    std::atomic<bool> compress_requested_{false};

    // §AutoCompress: turn guard. run_one_turn() holds turn_mutex_ for the
    // whole turn; turn_active_ mirrors that for same-thread checks so
    // compress_context() can compress immediately when idle without
    // try_lock UB on the turn's own thread.
    std::mutex turn_mutex_;
    std::atomic<bool> turn_active_{false};

    /// @brief Compress immediately on the caller's thread. Caller must
    ///        hold turn_mutex_ (or be inside a turn). on_compaction fires
    ///        for both Compressed and FallbackDeleted outcomes — the
    ///        fallback delete is a real context reduction and hosts must
    ///        see it (§COMPRESS-REPORT).
    CompressOutcome compress_now_unlocked();

    // §fix (2026-07-30, hardened 2026-08-01): circuit breaker for repeated
    // FATAL API errors. EVERY fatal-classified cycle increments the counter
    // (not just identical signatures — alternating 400 shapes each inject a
    // recovery message that corrupts the history into the next error shape,
    // so a same-signature check never tripped). Once the count reaches 2 we
    // stop returning ACT (which just re-sends the same doomed request and
    // appends another [SYSTEM ERROR] user message) and end the turn instead.
    // Reset on any successful API response.
    std::string last_fatal_signature_;  // kept for logging
    int consecutive_fatal_count_ = 0;
    // §2026-09-30: set when the fatal breaker ENDS a turn. Without this the
    // turn returned AgentState::DONE with no error anywhere — a muted agent
    // (every API call 400ing, e.g. a poisoned tool schema) looked identical
    // to a clean completion at the hub surface (done:true, error:"").
    bool ended_on_fatal_breaker_ = false;
    std::string fatal_breaker_error_;

    // §IdleExitBreaker (2026-08-20): consecutive agent-loop iterations
    // whose ONLY tool call is set_status. set_status only reports state
    // to the host UI — it NEVER exits the loop (§IdleLoopFix made the
    // tool reject runtime-owned states and spell this out), yet models
    // still end turns with set_status("Done") instead of the stop tool
    // and then answer every re-invocation with another set_status,
    // looping forever. Two in a row = the model is clearly trying to
    // exit, so the breaker ends the turn for it (same as stop).
    // Reset at turn start, on any non-set_status tool call, and on
    // plain-text responses.
    int consecutive_status_only_count_ = 0;
    // Phase 65e: voluntary compress_context rate limit — Honey habitually
    // self-compressed at 49% mid-storm (2026-09-16), wiping working memory
    // and re-answering echoes. Refuse below 70% real pressure and at most
    // one voluntary compress per kVoluntaryCompressCooldownS.
    std::chrono::steady_clock::time_point last_voluntary_compress_{};
    static constexpr int kVoluntaryCompressCooldownS = 600;

    // §BUG-COMPRESS-LOOP (2026-08-10): consecutive compression attempts
    // that did NOT get the context back below the auto-compress threshold.
    // The 90% auto-compress branch used to retry forever: every attempt
    // reset iteration_count_ (so the max-iterations guard could never
    // fire) and `continue`d the loop, so a compression that cannot bring
    // the context under the threshold — summary call keeps failing,
    // history too small to reduce, a single tool result bigger than the
    // window, or a max_context_size configured far smaller than the real
    // model window — looped "Compressing" indefinitely, each failed
    // attempt blocking up to ~210s in internal retries (30s/60s/120s).
    // Incremented on every compression attempt in the run loop, reset on
    // any cycle that completes below the threshold; the loop ends the
    // turn with a terminal on_error once the cap is exceeded.
    int consecutive_compress_count_ = 0;

    /**
     * Record a FATAL-classified API error and decide whether the turn must
     * end. Returns true once the same turn has hit kMaxConsecutiveFatal
     * consecutive fatal errors — callers then emit a terminal on_error and
     * return AgentState::DONE WITHOUT injecting another recovery message.
     */
    bool fatal_breaker_should_end_turn(const std::string& signature);
    bool status_only_breaker_should_end_turn(bool exit_requested);  // §IdleExitBreaker

    // §State Machine: LLM-driven transitions (via the `set_status` tool)
    // are reported through this callback so the owning AgentImpl can keep
    // its atomic public state as the single source of truth (Agent::state()
    // + AgentSink::on_state_change with the correct prev). When unset, the
    // fallback fires sink_->on_state_change directly with locally tracked
    // prev — preserving behaviour for direct LLMClient users.
    StateChangeCallback state_change_cb_;
    int fallback_prev_public_state_ = 0;  // hive::AgentState int value

    // §Phase 5.1: cancellation flows through this stop_source.
    // Agent::cancel() → LLMClient::cancel() → cancel_source_.request_stop().
    // The API wait loop and the per-iteration check both consult
    // cancel_source_.get_token().stop_requested().
    std::stop_source cancel_source_;

    // MCP legacy path removed. The MCP system is now driven entirely by
    // McpServerManager + McpProxiedTool (Agent.md §18, §18.9).
    // See ToolHandler::get_tool_definitions() — which now includes each
    // server's tools as native first-class tools — for the LLM-visible
    // surface.

    // §Phase 11: injected McpServerManager* instead of ::instance() singleton.
    // nullptr means mcp_enabled=false — all MCP operations become no-ops.
    mcp::McpServerManager* mcp_manager_ = nullptr;

    // §Phase 16: per-agent SkillManager. nullptr means no skills configured.
    SkillManager* skill_manager_ = nullptr;

    // §Phase 16: per-agent tool filter. nullptr means all tools allowed.
    const ToolFilter* tool_filter_ = nullptr;

    // Tracks whether McpServerManager has signalled that the MCP tool
    // surface needs rebuilding. Set true when mcp_tools_dirty() returns true.
    bool mcp_tools_refresh_pending_ = true;

    // §READ-IMAGE-CORE (2026-08-10): images loaded by the read_image tool
    // during the current turn, waiting to be attached to the next chat
    // request as image_url content parts (Kimi/OpenAI vision format).
    // The data URI rides the request payload only — stored history keeps
    // just the stripped text reference (see the read_image special-case
    // in run_agent_cycle's tool-result handling).
    struct PendingImage {
        std::string path;
        std::string prompt;
        std::string data_uri;
        std::string via;  ///< "read_image" | "user attachment" (injection label)
    };
    std::vector<PendingImage> pending_images_;

    /// Queue one image ref (local path / http(s) URL / data URI) for
    /// payload-only injection on the next request. Load failures are
    /// logged and skipped — they never abort the turn.
    void queue_image_ref(const std::string& ref, const std::string& via);

    /// True when the active provider plugin's static model table marks
    /// config_.model as vision-capable. Unknown models (not in the
    /// table) are treated as vision-capable — the API is the arbiter.
    [[nodiscard]] bool model_supports_vision() const;

    // State machine methods
    // §NC-8 (2026-07-16): CURL* parameter dropped. The sync fallback
    // path now uses OmniNet::HTTP::Client internally.
    AgentState run_agent_cycle(const std::string& api_url, json& messages, const json& tool_defs);
    [[nodiscard]] std::string get_state_name(AgentState state) const;
    [[nodiscard]] std::string extract_error_message(const std::string& response) const;

    // §on_error: route a structured error to the host via
    // AgentSink::on_error. Every error is also written unconditionally to the
    // file Logger, so hosts that override on_error/on_log still leave a trace
    // in the project log file. `will_retry` tells the host whether the
    // library will attempt the request again.
    void emit_error(hive::ErrorKind kind, const std::string& message, bool will_retry);

    // Buffer management
    // Truncate messages if they exceed max_request_size (keeps system + recent messages)
    json truncate_messages_if_needed(json& messages, size_t max_size);

    // Helper to build multi-modal user messages
    // Detects image references in the format ![alt](path_or_url) and converts to array content
    
    // Extract and store token usage from a parsed LLM response.
    // Called after every successful API response so that
    // last_total_tokens_ / last_prompt_tokens_ / last_completion_tokens_
    // stay up-to-date for the stats display and compression threshold.
    void update_token_usage(const json& response_json);

    // §Phase 9.10: called by the compression pipeline (Messages::compress /
    // call_llm_for_summary) after its non-streaming LLM call succeeds.
    // The compression call gets real usage counts (MiniMax SSE is always null);
    // we mirror them here so the main agent's token bar stays accurate between
    // streaming turns. Thread-safe for the compression thread calling this
    // while the main thread reads last_total_tokens_ — the staleness of
    // ~1 second is acceptable for a progress indicator.
    void report_compression_token_usage(int prompt_tokens, int completion_tokens, int total_tokens) {
        last_prompt_tokens_     = prompt_tokens;
        last_completion_tokens_ = completion_tokens;
        last_total_tokens_     = total_tokens;
    }

    // Helper to get max context size (from messages if available, else default)
    size_t get_max_context_size_value() const {
        if (messages_) {
            return messages_->get_config().max_context_size;
        }
        return 200000;
    }

public:
    // §Phase 11: mcp_manager added (injected instead of ::instance()).
    // §v0.5.0: settings_manager replaced with settings_store (SettingsStore*).
    explicit LLMClient(const AppConfig& config,
                       hive::SettingsStore* settings_store = nullptr,
                       mcp::McpServerManager* mcp_manager = nullptr,
                       SkillManager* skill_manager = nullptr,
                       const ToolFilter* tool_filter = nullptr);

    /// @brief True when the last turn was ENDED by the fatal-error circuit
    ///        breaker (unrecoverable repeated API errors). When true,
    ///        fatal_breaker_error() carries a human-readable cause — hosts
    ///        must surface it as the turn error instead of reporting a
    ///        clean completion (2026-09-30 silent-mute incident).
    bool ended_on_fatal_breaker() const { return ended_on_fatal_breaker_; }
    const std::string& fatal_breaker_error() const { return fatal_breaker_error_; }

    /// @brief Name of the resolved provider plugin (§ProviderPlugins).
    ///        Empty only before the ctor resolves it.
    std::string provider_plugin_name() const {
        return plugin_ ? plugin_->name() : std::string();
    }

    /// @brief Thinking levels the current model honors, per the resolved
    ///        plugin. Empty = no thinking control (or no plugin yet).
    std::vector<hive::ThinkingLevel> supported_thinking_levels() const {
        return plugin_ ? plugin_->supported_thinking_levels(config_.model)
                       : std::vector<hive::ThinkingLevel>{};
    }

    /// @brief Runtime thinking-level toggle (§ThinkingLevel). The payload
    ///        hook reads config_.thinking_level on every chat request, so
    ///        the new level applies from the next turn — call between
    ///        turns, not mid-request.
    void set_thinking_level(hive::ThinkingLevel level) {
        config_.thinking_level = level;
    }

    /// @brief Currently configured thinking level.
    hive::ThinkingLevel thinking_level() const {
        return config_.thinking_level;
    }

    /// @brief Host-facing context compression (§AutoCompress). When no
    ///        turn is running, compresses immediately (synchronously on
    ///        the caller's thread; on_compaction fires from that thread).
    ///        When a turn is running, sets a flag consumed at the
    ///        compression checkpoint at the end of the current agent
    ///        cycle — honored even if the turn is cancelled. If there is
    ///        nothing to compress yet, the request carries over to the
    ///        next turn's first cycle. Thread-safe.
    void compress_context() noexcept;

    /// @brief Attach a sink. Call this after construction and before
    ///        initialize(). Required for streaming output, log routing, and
    ///        blocking I/O (bash approval).
    ///        Also propagates the sink to ToolHandler so execute_bash can
    ///        call request_command_approval when not auto-approved.
    void set_sink(std::shared_ptr<hive::AgentSink> sink) {
        sink_ = sink;
        ToolHandler::set_sink(std::move(sink));
    }

    /// @brief Wire the LLM-driven public-state transition report. When set,
    ///        `set_status` tool results are forwarded here instead of firing
    ///        the sink directly, so the owner (AgentImpl) stays the single
    ///        source of truth for the public AgentState.
    void set_state_change_callback(StateChangeCallback cb) {
        state_change_cb_ = std::move(cb);
    }

    /// @brief Attach the per-agent SkillManager after construction.
    ///        §SKILL-ORDER fix: AgentImpl constructs the LLMClient before
    ///        the SkillManager exists, so the constructor always received
    ///        nullptr and skill_manager_ stayed null for the agent's
    ///        lifetime (SkillManager::current() null inside every turn,
    ///        no skills catalog in the system prompt). AgentImpl calls
    ///        this once the manager has been created. Must be called
    ///        before any turn runs.
    void set_skill_manager(SkillManager* skill_manager) {
        skill_manager_ = skill_manager;
    }

    /// @brief True when a SkillManager is attached (testing/diagnostics
    ///        for the §SKILL-ORDER wiring).
    [[nodiscard]] bool has_skill_manager() const noexcept {
        return skill_manager_ != nullptr;
    }

    /// @brief Handle a tool result that may carry a `set_status` state
    ///        transition ({"status":"set_state","state":"<Name>"}).
    ///        Dispatches via state_change_cb_ (or the sink fallback) and
    ///        returns true when a valid transition was dispatched.
    ///        Extracted from run_agent_cycle() so it is unit-testable.
    bool handle_set_state_result(const json& result);

    /// @brief Handle a tool result from the model-facing
    ///        `compress_context` tool ({"status":"compress_context",...}).
    ///        Sets the same compress_requested_ flag as
    ///        compress_context(), so compression runs at the
    ///        cycle-boundary checkpoint (§AutoCompress) — never inline,
    ///        never interrupting in-flight tool calls. Returns true when
    ///        the result was a valid compression request.
    bool handle_compress_context_result(const json& result);

    /// @brief Update the running AppConfig (e.g., after a model switch).
    ///        Also refreshes the Messages compression config so context-size
    ///        and API settings follow the new model.
    void set_config(const AppConfig& config);

    /// @brief Clear the conversation history and persist the empty state.
    ///        Called when switching models so the new model starts with a
    ///        fresh context.
    void clear_messages();

    // Refresh the MCP tool surface from McpServerManager. Call this at
    // startup and any time the manager's dirty flag flips (the LLMClient
    // tracks the flag in mcp_tools_refresh_pending_ and calls this
    // automatically at the start of each agent cycle).
    void refresh_mcp_tools();

    // API interaction functions

    // §robust-client: result of one HTTP round-trip to the chat endpoint.
    // Carries the HTTP status alongside the body so error handling can key
    // on the status code instead of trying to json::parse an (often empty,
    // in streaming mode) error body. status_code == 0 means the request
    // never got an HTTP response (transport failure — in that case an
    // exception is thrown instead, so 0 should not normally be observed).
    struct ApiResponse {
        long        status_code = 0;
        std::string body;   // response body; the provider's error document on >= 400

        [[nodiscard]] bool ok() const noexcept {
            return status_code >= 200 && status_code < 300;
        }
    };

    // §NC-8 (2026-07-16): CURL* parameter dropped. Implementation
    // uses OmniNet::HTTP::Client::send_blocking.
    ApiResponse make_http_request_with_retry(const std::string& url, const std::string& payload, int max_retries = 3);

    // Async API request using HttpSession with optional cancellation.
    // Returns a future to an ApiResponse (HTTP status + body). Throws
    // std::runtime_error on permanent failure (e.g., all retries exhausted).
    //
    // When streaming_callback is provided, the Accept header is set to
    // SSE and chunks are delivered to the callback as they arrive. The
    // caller is responsible for SSE-parsing and calling
    // sink_->on_assistant_delta() per text delta. On HTTP error status
    // (>= 400) the callback is NOT invoked and the error body is
    // accumulated into ApiResponse::body instead.
    std::future<ApiResponse> make_api_request_async(
        const std::string& url,
        const std::string& payload,
        std::stop_token stop_token = {},
        std::function<void(std::string_view)> streaming_callback = nullptr
    );
    AgentState run_one_turn(const std::string& user_message);  // Phase 2.5: extracted agent cycle (returns when agent emits stop/cancel)

    /// §READ-IMAGE-CORE follow-up (2026-08-10): queue host-supplied image
    /// attachments for the next chat request. Shares the read_image
    /// pipeline: images ride the request payload only (stored history
    /// keeps plain text) and injection is gated on model_supports_vision().
    /// Call BEFORE run_one_turn(); Agent::submit_user_message(text, imgs)
    /// is the public-API path that lands here.
    void queue_image_attachments(const std::vector<hive::ImageAttachment>& images);
    // Note: the old run_interaction() / run_manager_mode() have been
    // removed. The Agent has exactly ONE loop. Manager / sub-agent
    // patterns are host concerns and will live in demos/daedalus-manager/
    // and demos/daedalus-subagent/.

    // Phase 2.7: lifecycle hooks. Renamed to match prompt.txt
    // ("easy cleanup() function"). The dtor also calls cleanup()
    // automatically; explicit calls are idempotent.
    //
    // Typical host usage:
    //   LLMClient client(cfg, settings);
    //   client.initialize();                        // one-time setup
    //   while (true) {
    //       std::string input = host.read_input();
    //       if (input == "/quit") break;
    //       AgentState final = client.run_one_turn(input);
    //       // host may inspect final, dispatch, etc.
    //   }
    //   client.cleanup();                           // explicit cleanup
    //   // ~LLMClient() also calls cleanup() if not already done
    void initialize();  // one-time setup: banner, persisted messages
    void cleanup();     // graceful teardown: save messages + session memory

    // §CRASH-4 (2026-07-16): forward crash-handler install/uninstall
    // to the underlying Messages instance. Idempotent.
    void install_crash_handlers();
    void uninstall_crash_handlers();

    // §Phase 5.1: cancel an in-flight turn. Fires cancel_source_, which
    // the API wait loop and the per-iteration check both consult. Safe
    // to call from any thread. Idempotent. Hosts that want ESC-to-cancel
    // capture ESC themselves and call this method (see
    // docs/embedding.md §"Wiring your own cancellation").
    void cancel() noexcept;
    
    // Stats getters for user display
    int get_total_tokens() const { return last_total_tokens_; }
    int get_prompt_tokens() const { return last_prompt_tokens_; }
    int get_completion_tokens() const { return last_completion_tokens_; }
    // §ContextCache: tokens served from the provider's prompt cache on
    // the last response. Both wire shapes are folded into this one
    // counter: Moonshot's top-level usage.cached_tokens and the
    // OpenAI-style usage.prompt_tokens_details.cached_tokens (MiniMax).
    int get_cached_tokens() const { return last_cached_tokens_; }
    int get_max_context_size() const {
        if (messages_) {
            return static_cast<int>(messages_->get_config().max_context_size);
        }
        return 200000; // Default fallback
    }
    int get_iteration_count() const { return iteration_count_; }
    int get_max_iterations() const { return max_iterations_; }
    [[nodiscard]] int get_message_count() const noexcept {
        return messages_ ? static_cast<int>(messages_->size()) : 0;
    }

    /// §Phase 6: return the body of the most recent assistant message,
    /// or empty string if no assistant turn has completed yet. Hosts use
    /// this to capture the agent's final answer (e.g. composition
    /// patterns like spawn-a-sub-agent).
    std::string get_last_assistant_text() const {
        if (messages_) return messages_->get_last_assistant_content();
        return {};
    }
    
    // Inject context statistics into system message before API calls
    // This allows the agent to see token usage, memory stats, etc.
    void inject_context_stats(json& messages);
    
    // Session Memory Event Hooks
    void on_build_complete(const std::string& command, bool success) {
        if (session_memory_) session_memory_->on_build_complete(command, success);
    }
    void on_build_error(const std::string& error, const std::string& attempted_fix = "") {
        if (session_memory_) session_memory_->on_error(error, attempted_fix);
    }
    void on_error_resolved(const std::string& error, const std::string& solution) {
        if (session_memory_) session_memory_->on_error_resolved(error, solution);
    }
    void on_user_correction(const std::string& context, const std::string& correction) {
        if (session_memory_) session_memory_->on_user_correction(context, correction);
    }
    std::string get_session_memory_prompt() const {
        if (session_memory_) return session_memory_->get_system_prompt_addition();
        return "";
    }
    void end_session(const std::string& summary, const std::vector<std::string>& files = {}, int iterations = 0) {
        if (session_memory_) session_memory_->on_session_end(summary, files, iterations);
    }
};