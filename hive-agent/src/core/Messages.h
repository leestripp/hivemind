/**
 * @file Messages.h
 * @brief Unified message handling and compression for hive-brain
 * 
 * This class handles:
 * - Message storage (json array operations)
 * - Message persistence (save/load to JSON file)
 * - Message compression (compress middle via LLM)
 * - Tool call lifecycle management (state derived from message chain)
 * 
 * ARCHITECTURE: Single Source of Truth
 * Tool call state is DERIVED from the message chain, not stored separately:
 * - PENDING: assistant message has tool_calls[X].id="abc", no role="tool",tool_call_id="abc" exists
 * - EXECUTING: runtime marker only (ephemeral, not persisted) - stored in executing_tools_ set
 * - COMPLETED: role="tool",tool_call_id="abc" exists with non-error content
 * - FAILED: role="tool",tool_call_id="abc" exists with error content in "content"
 * - INVALID: assistant message has tool_calls with malformed JSON arguments
 * 
 * Refactored from MiniMaxClient and ContextCompressor to consolidate
 * all message-related functionality in one place.
 */

#pragma once

#include <string>
#include <filesystem>
#include <optional>
#include <unordered_set>
#include <nlohmann/json.hpp>
#include <future>
#include <thread>
#include <chrono>
#include <mutex>
#include <condition_variable>
#include <atomic>

using json = nlohmann::json;

// ============================================================================
// Configuration & Result Structs
// ============================================================================

/**
 * @brief Message compression configuration
 */
struct MessageCompressionConfig {
    // Token management
    size_t max_context_size = 200000;        // API token limit
    float compress_threshold = 0.9f;          // Trigger at 90% of max
    
    // Compression output
    size_t max_compress_output = 1500;        // Max tokens in LLM summary

    // Split ratio: compress the FIRST (compress_ratio * 100)% of messages,
    // keep the LAST ((1 - compress_ratio) * 100)% intact.
    // Default: compress oldest 70%, keep newest 30%
    float compress_ratio = 0.70f;             // Compress first 70%, keep last 30%
    
    // What to keep intact (messages, not including system at index 0)
    // Note: keep_head_count and keep_tail_count are DEPRECATED - we now compress ALL non-system messages
    // The compressed history rebuilds as:
    //   system + user(synthetic message carrying the summary —
    //   §COMPRESS-USER-MSG: strict templates require the first message
    //   after system to be a user msg) + suffix(newest 30%)
    // Keeping these as placeholders (not used) for backward compatibility with tests
    int keep_head_count = 2;                  // DEPRECATED - not used anymore
    int keep_tail_count = 2;                 // DEPRECATED - not used anymore
    
    // API settings
    std::string model = "MiniMax-M2.7";
    std::string api_url;
    std::string api_key;
    double temperature = 0.0;  // §Phase 8.7: sampling temperature for summary LLM.
                               // §BUG-COMPRESS-TEMP: MUST be pre-clamped by the
                               // owner (LLMClient) via ProviderPlugin::
                               // clamp_temperature() — Messages never sees the
                               // plugin, and fixed-temperature models reject
                               // unclamped values.
    // §COMPRESS-REPORT: base timeout in seconds for the compression API
    // call. call_llm_for_summary() tries 3 times with growing timeouts:
    // compression_timeout, 2x, 4x (default 30 → 30s/60s/120s).
    int compression_timeout = 30;             // Base seconds; retries use 2x/4x
    
    // Debug
    bool debug_output = false;

    // §CRASH-2 (2026-07-16): write-through auto-flush. When enabled
    // (default), every add_*() marks the messages as dirty and a
    // background thread flushes to disk within ~100 ms. Guarantees
    // that an abrupt exit never loses more than one turn's worth of
    // state. Disable in tests that want full control over save()
    // timing or want to assert that save() is not called by add_*.
    bool auto_flush_enabled = true;

    // §CRASH-2: flush interval in milliseconds. Default 100 ms gives
    // a reasonable balance between I/O overhead and data-loss
    // window. Tests with strict timing can dial this down to 0
    // (flush on every dirty mark).
    int auto_flush_interval_ms = 100;

    // §Phase 9.10: optional callback invoked by call_llm_for_summary after a
    // successful compression API call. Reports (prompt_tokens, completion_tokens,
    // total_tokens) to the main LLMClient so the agent's token bar stays
    // accurate between streaming turns (SSE is always usage=null).
    // Signature: void(int, int, int) — set by LLMClient::initialize().
    std::function<void(int, int, int)> on_compression_tokens;
};

/**
 * @brief Outcome of a Messages::compress() call (§COMPRESS-REPORT).
 *
 * The old std::optional<std::string> return conflated three different
 * endings into a single nullopt — "nothing to compress", "summary
 * failed but the fallback deleted the oldest messages anyway", and
 * "gave up entirely". Hosts keying success off on_compaction therefore
 * misreported a real context reduction as a failure, because the
 * fallback-delete path never surfaced as a state change.
 */
enum class CompressOutcome {
    Compressed,         ///< Summary generated; CompressResult::compressed_json is valid.
    NothingToCompress,  ///< History too small / empty middle; nothing changed.
    FallbackDeleted,    ///< Summary failed; oldest ~70% deleted as last resort.
                        ///< The context WAS reduced — callers must still fire
                        ///< on_compaction and must NOT schedule a retry.
    Failed,             ///< Summary failed AND history too small to safely reduce.
};

/**
 * @brief Return value of Messages::compress().
 */
struct CompressResult {
    CompressOutcome outcome = CompressOutcome::Failed;
    std::string compressed_json;  ///< Valid only when outcome == Compressed.
};

/**
 * @brief Result of a compression operation
 */
struct CompressionResult {
    json prefix_messages;           // First N messages after system (kept)
    std::string middle_summary;    // LLM-generated summary of middle
    json suffix_messages;          // Last N messages (kept - current state)
    size_t original_count = 0;     // Original message count
    size_t compressed_count = 0;   // New message count (prefix + 1 + suffix)
    size_t original_tokens = 0;    // Estimated tokens before
    size_t compressed_tokens = 0;   // Estimated tokens after
};

// ============================================================================
// Phase 11: removed SubAgentCompressor class + SubAgentCompressionResult
// struct. They wrapped the 'hive-brain --sub-agent' subprocess path which
// called a CLI flag removed in Phase 2.7. The synchronous compression
// path (Messages::compress_messages → Messages::call_llm_for_summary)
// is the only path used in production.

// ============================================================================
// Messages Class
// ============================================================================

// §CRASH-6 (2026-08-05): crash-mirror state shared with the signal
// handler. Defined in Messages.cpp; heap-allocated per Messages
// instance and intentionally leaked for process lifetime so the
// signal handler can never touch freed memory.
struct MessagesCrashState;

class Messages {
public:
    /**
     * @brief Construct Messages with a per-agent data directory (§Phase 8.8).
     * @param data_dir Fully-resolved namespace directory — either
     *                 `<project_root>/.daedalus/hive-brain/` (empty agent_name) or
     *                 `<project_root>/.daedalus/daedalus-<name>/` (named agent).
     *                 The directory is auto-created if missing.
     *                 AgentImpl calls resolve_agent_dir(project_root,
     *                 cfg.agent_name) to compute this; Messages
     *                 itself does NOT know about agent_name.
     * @param config Compression configuration
     */
    Messages(std::filesystem::path data_dir,
             const MessageCompressionConfig& config = MessageCompressionConfig());
    
    ~Messages();

    // §CRASH-2 (2026-07-16): Messages holds std::atomic<bool> and
    // std::thread, both of which are NOT movable. The default move
    // ops would be deleted by the compiler with a confusing error
    // message, so we delete them explicitly with a clear comment.
    // Tests rely on RVO when returning Messages by value from
    // factory helpers — RVO does NOT require a move constructor.
    Messages(const Messages&) = delete;
    Messages& operator=(const Messages&) = delete;
    Messages(Messages&&) noexcept = delete;
    Messages& operator=(Messages&&) noexcept = delete;

    // =========================================================================
    // Message Access
    // =========================================================================
    
    /**
     * @brief Get direct reference to message array (for API calls, etc.)
     *
     * §PERSIST-RACE (2026-09-02): the returned reference is NOT
     * synchronized. It exists for the single-threaded LLMClient turn
     * path, which must call mark_dirty() after each mutation batch so a
     * consistent snapshot is published for the flush worker. The worker
     * never reads messages_ directly (see persist_snapshot_ below), so
     * unlocked mutation through this reference cannot tear a worker's
     * deep copy (MonkeyByte SIGSEGV, daedalus_bugs.md BUG 1).
     */
    json& get_messages() { return messages_; }
    const json& get_messages() const { return messages_; }
    
    /**
     * @brief Get message count
     */
    size_t size() const { return messages_.size(); }
    bool empty() const { return messages_.empty(); }
    
    /**
     * @brief Get system message content
     */
    std::string get_system_content() const;
    
    /**
     * @brief Get last assistant message content
     */
    std::string get_last_assistant_content() const;
    
    /**
     * @brief Get last assistant message (for adding tool_calls)
     */
    json& get_last_assistant_message() { return messages_.back(); }
    const json& get_last_assistant_message() const { return messages_.back(); }

    // =========================================================================
    // Message Operations
    // =========================================================================
    
    /**
     * @brief Add system message (only if messages empty)
     */
    void add_system(const std::string& content);
    
    /**
     * @brief Add user message (supports multi-modal with image_url)
     */
    void add_user(const std::string& content, const std::string& image_url = "");
    
    /**
     * @brief Add assistant message
     */
    void add_assistant(const std::string& content);
    
    /**
     * @brief Add tool result message
     * @param tool_call_id The ID of the tool call this result is for
     * @param content The result content
     * @param is_error Whether this is an error result (defaults to false)
     */
    void add_tool_result(const std::string& tool_call_id, const std::string& content, bool is_error = false);
    
    /**
     * @brief Add raw message (for loading from file, etc.)
     */
    void add_message(const json& msg);
    
    /**
     * @brief Set messages array directly (used by LLMClient to sync)
     */
    void set_messages(const json& msgs);

    // =========================================================================
    // Persistence
    // =========================================================================
    
    /**
     * @brief Load messages from .daedalus/hive-brain[-<name>]/messages.json
     * @return true if loaded successfully
     */
    bool load();
    
    /**
     * @brief Save messages to .daedalus/hive-brain[-<name>]/messages.json
     */
    void save() const;

    /**
     * @brief §CRASH-2 (2026-07-16): synchronously flush any pending
     * dirty messages to disk. Returns immediately if not dirty.
     *
     * Use this BEFORE dispatching an LLM call if you want the user's
     * prompt to be persisted before the LLM touches anything: if the
     * LLM call hangs or crashes, the user's input is already on
     * disk.
     */
    void flush_now() const;

    /**
     * @brief §CRASH-2: non-blocking flush. Returns true if the
     * worker thread will flush soon (within auto_flush_interval_ms).
     * Returns false if auto-flush is disabled.
     */
    bool flush_if_dirty() const;

    /**
     * @brief Mark the chain dirty so the background flush worker (and the
     * destructor's final flush) persist it. Needed because the LLMClient
     * turn path mutates the chain through the raw get_messages()
     * reference - bypassing add_*() - which used to leave dirty_ unset
     * for entire sessions: nothing was written between cleanup() calls,
     * the .crash mirror went stale, and hosts that never call
     * Agent::shutdown() lost the whole session (or reloaded a stale
     * chain ending in an unanswered user message, which the next
     * session then re-answered alongside the new prompt).
     * (\u00a7PERSIST-DIRTY, 2026-08-31)
     *
     * §PERSIST-RACE (2026-09-02): this also PUBLISHES a deep copy
     * of the chain into persist_snapshot_ (under messages_mutex_). The
     * flush worker saves only that published copy, so it never races
     * raw-reference mutations of messages_. Call it AFTER a mutation
     * batch completes, never mid-mutation.
     */
    void mark_dirty();

    /**
     * @brief Clear all messages and persist the cleared state
     *
     * Resets the in-memory chain, then synchronously removes the
     * persisted state (messages.json, the .tmp/.crash mirrors, and the
     * SQLite mirror row) so a cleared session cannot be resurrected on
     * the next load() (§CRASH-6, monkeybyte report defect #6).
     */
    void clear();

    /**
     * @brief Remove an incomplete trailing tool cycle (assistant message with
     *        tool_calls followed by tool results) so a new user message can be
     *        appended. Returns true if any messages were removed.
     */
    bool cleanup_dangling_tool_results();

    /**
     * @brief §CRASH-4 (2026-07-16): async-signal-safe crash handler.
     *
     * Installs SIGSEGV / SIGABRT / SIGFPE / SIGILL handlers that
     * write the current messages to messages.json.crash using
     * only async-signal-safe primitives (open(2), write(2), close(2)),
     * then re-raise the signal so the user still gets their core
     * dump / stack trace.
     *
     * Idempotent: calling twice does not stack handlers.
     *
     * The crash handler MUST be installed after a Messages object is
     * alive (so its pre-opened fd exists). A fixed-size process-global
     * registry of per-instance crash-mirror states keeps the list of
     * active instances; the signal handler iterates it lock-free.
     * The states are intentionally leaked for process lifetime, so the
     * handler can never dereference freed memory (§CRASH-6).
     *
     * Call this from main() (after constructing the Agent / Messages)
     * and pair it with `uninstall_crash_handlers()` in the graceful
     * shutdown path so the signal handler does not iterate a
     * half-destroyed list.
     */
    void install_crash_handlers();
    void uninstall_crash_handlers();

    // §CRASH-6 (2026-08-05): snapshot of the crash-mirror payload (the
    // JSON blob the signal handler would write to messages.json.crash).
    // Returns a copy; safe to call from normal context. The signal
    // handler itself reads the double-buffered MessagesCrashState
    // directly — it does not go through this accessor.
    std::string crash_payload_for_signal() const;
    /**
     * @brief Check if messages file exists
     */
    bool has_saved_messages() const;

    // =========================================================================
    // Compression
    // =========================================================================
    
    /**
     * @brief Check if compression should be triggered (use in main loop only)
     * @param current_tokens Current token count from API
     * @return true if at or above compress_threshold
     */
    bool should_compress(size_t current_tokens) const;
    
    /**
     * @brief Compress middle messages - just compress, no threshold check
     *        (threshold check should be done in main loop before calling this)
     * @return CompressResult whose outcome distinguishes success, nothing-to-
     *         compress, fallback-delete (context reduced WITHOUT a summary),
     *         and outright failure. compressed_json is valid only when
     *         outcome == CompressOutcome::Compressed.
     */
    CompressResult compress();
    
    /**
     * @brief Replace messages array with compressed messages from JSON string
     * @param compressed_json Output from compress()
     */
    void replace_messages(const std::string& compressed_json);

    /**
     * @brief Truncate messages if they exceed max_size (keeps system + recent)
     * @param max_size Maximum serialized size in bytes
     * @return JSON object with "truncated" flag and size info, or empty if no truncation
     */
    json truncate_if_needed(size_t max_size);

    /**
     * @brief Get compression ratio
     */
    float get_compression_ratio(size_t current_tokens) const;

    // =========================================================================
    // Helpers
    // =========================================================================
    
    /**
     * @brief Estimate total tokens in messages (rough: size/4)
     */
    size_t estimate_tokens() const;
    
    /**
     * @brief Inject context stats into system message
     * @param current_tokens Current token count
     * @param iteration Current iteration number
     */
    void inject_context_stats(size_t current_tokens, size_t iteration, size_t max_iterations);
    
    /**
     * @brief Get project root path
     */
    const std::filesystem::path& get_project_root() const { return data_dir_; }
    
    /**
     * @brief Get compression config
     */
    const MessageCompressionConfig& get_config() const { return config_; }

    /**
     * @brief Update compression config (used when switching models)
     *
     * Updates fields needed for context size + compression API calls:
     * max_context_size, model, api_url, api_key. Other fields are preserved.
     */
    void set_config(const MessageCompressionConfig& new_config) { config_ = new_config; }
    
    /**
     * @brief Get split info for testing and debugging
     */
    void get_split_info(size_t& prefix_size, size_t& middle_size, size_t& suffix_size) const;
    /**
     * @brief Get indices of first half messages that will be deleted and compressed
     * @return pair of (start_index, end_index_exclusive) in messages_ array
     *         Returns (0, 0) if no messages to compress
     */
    std::pair<size_t, size_t> get_compression_indices() const;

    // =========================================================================
    // Tool Call Lifecycle Management (Single Source of Truth)
    // =========================================================================
    
    /**
     * @brief Tool call states (derived from message chain, except EXECUTING)
     * 
     * State is determined by the message chain:
     * - PENDING: assistant message has tool_calls[X].id="abc", no tool result exists
     * - EXECUTING: runtime marker only (ephemeral) - stored in executing_tools_
     * - COMPLETED: role="tool",tool_call_id="abc" exists with non-error content
     * - FAILED: role="tool",tool_call_id="abc" exists with error content
     * - INVALID: assistant message has tool_calls with malformed JSON arguments
     */
    enum class ToolState {
        PENDING,    // LLM requested tool call, not yet executed
        EXECUTING,  // Tool is currently running (ephemeral runtime state)
        COMPLETED,  // Tool succeeded with result
        FAILED,     // Tool failed with error
        INVALID     // Arguments malformed, cannot execute
    };
    
    /**
     * @brief Mark a tool call as executing (ephemeral runtime state)
     * @param id tool_call_id to mark
     * @return true if found and marked, false if not found
     */
    bool mark_tool_executing(const std::string& id);
    
    /**
     * @brief Clear EXECUTING state for a specific tool (called when tool completes)
     * @param id tool_call_id to clear from executing set
     */
    void clear_executing(const std::string& id);
    
    /**
     * @brief Check if a tool is currently in EXECUTING state (ephemeral)
     * @param id tool_call_id to check
     * @return true if in executing_tools_ set
     */
    bool is_executing(const std::string& id) const;
    
    /**
     * @brief Derive tool state from message chain
     * @param id tool_call_id to check
     * @return current state of the tool call
     */
    ToolState get_tool_state(const std::string& id) const;
    
    /**
     * @brief Check if all tool calls have been resolved
     * @return true if all pending or executing have been resolved
     */
    bool all_tools_resolved() const;
    
    /**
     * @brief Check if there are any pending tool calls
     * @return true if there are PENDING tool calls
     */
    bool has_pending_tools() const;
    
    /**
     * @brief Check if there are any executing tool calls
     * @return true if there are EXECUTING tool calls (ephemeral)
     */
    bool has_executing_tools() const;
    
    /**
     * @brief Get all pending tool call IDs
     * @return vector of pending tool call IDs
     */
    std::vector<std::string> get_pending_tool_ids() const;
    
    /**
     * @brief Sanitize an assistant message (validate tool_calls arguments,
     *        prune tool_calls without a matching result in tool_result_ids)
     * @param assistant_msg assistant message to sanitize
     * @param tool_result_ids ids of tool results available to answer the
     *        calls (sanitize_message_chain passes only ADJACENT results)
     * @return sanitized assistant message
     */
    json sanitize_assistant_message(const json& assistant_msg,
                                        const std::unordered_set<std::string>& tool_result_ids = {}) const;
    
    /**
     * @brief Validate and sanitize entire message array (position-aware)
     *
     * Enforces the provider's tool-call contract: a tool result must sit in
     * the contiguous block directly after its parent assistant, may appear
     * only once, and every surviving tool_call must have an adjacent result.
     * Orphaned, misplaced, and duplicate tool results — and unanswered
     * tool_calls — are removed.
     *
     * @param messages message array to sanitize
     * @return sanitized message array
     */
    json sanitize_message_chain(const json& messages) const;

    /**
     * @brief Append synthetic tool results ONLY for a genuinely pending call
     *
     * §fix (2026-07-30): error-recovery paths use this to close an orphaned
     * tool_call. It injects results ONLY when the last assistant-with-
     * tool_calls is pending at the tail (every later message is a tool
     * result of the same block). Missing results are appended contiguously
     * (multi-tool safe); ids that already have a result are never
     * duplicated. A mid-history (already answered) call is left untouched,
     * because appending a tool result there would create the misplaced
     * tool message the provider rejects with HTTP 400.
     *
     * @param messages live chain (modified in place)
     * @param content content for any injected tool result
     * @return true if at least one tool result was appended
     */
    static bool append_tool_results_if_pending(json& messages,
                                               const std::string& content);
    
    /**
     * @brief Clear executing markers (called during compression)
     */
    void clear_executing_markers();

private:
    /**
     * @brief §fix (2026-07-18): unsynchronized persistence core. Caller
     * must hold save_mutex_. Factored out so flush_now() can wait for an
     * in-progress save and then flush any remaining dirty state.
     */
    void save_impl() const;

    /**
     * @brief §PERSIST-RACE: publish the current chain to
     * persist_snapshot_ and set dirty_. Caller MUST hold
     * messages_mutex_ and must be at a quiescent point (no mutation of
     * messages_ in flight). All internal mutators call this inside
     * their existing critical sections; the public mark_dirty() wraps
     * it with the lock for the raw-reference turn path.
     */
    void mark_dirty_locked();

    std::filesystem::path data_dir_;     // §Phase 8.8: per-agent namespace dir
    std::filesystem::path messages_file_;
    std::string session_id_ = "default"; // §Plan.md v0.3.5: SQLite mirror key
    json messages_;
    MessageCompressionConfig config_;

    // §PERSIST-RACE (2026-09-02): published copy of the chain,
    // refreshed by mark_dirty()/mark_dirty_locked() on the mutating
    // thread at quiescent points. save_impl() snapshots ONLY this -
    // never messages_ - because the turn path mutates messages_ through
    // the raw get_messages() reference without holding messages_mutex_,
    // and a deep copy racing those mutations is a torn read (json object
    // header with a null payload pointer -> SIGSEGV in the flush worker,
    // taking down the whole host process; daedalus_bugs.md BUG 1).
    // Guarded by messages_mutex_. mutable because save() is const.
    mutable json persist_snapshot_;

    // §fix (2026-07-18): guards messages_ against the background flush
    // worker. The worker calls save() concurrently with main-thread
    // add_*/set_messages/clear/compress; every mutation of messages_
    // holds this mutex, and save() takes its snapshot under it.
    // Read-only helpers called only from the main thread may stay
    // unlocked; private helpers never lock it themselves (the public
    // entry point does), so there is no recursive-lock hazard.
    mutable std::mutex messages_mutex_;

    // §fix (2026-07-18): serializes concurrent save() calls (flush
    // worker vs. a main-thread flush_now()) so the mutable
    // crash-mirror writes at the end of save() don't race.
    mutable std::mutex save_mutex_;

    // Ephemeral runtime state only (NOT persisted)
    // Tracks which tools are currently executing
    std::unordered_set<std::string> executing_tools_;

    // §CRASH-2 (2026-07-16): write-through auto-flush machinery.
    //
    // dirty_ is set by every add_*() and cleared by save(). The
    // background thread waits on flush_cv_ with a timeout of
    // auto_flush_interval_ms; when notified (or timed out) it checks
    // dirty_ and calls save() if set.
    //
    // stop_flush_ is set in the destructor to ask the worker to
    // exit. The worker flushes any final dirty state, joins
    // cleanly, and the destructor then proceeds to free resources.
    // This means the last add_*() before destruction is guaranteed
    // to hit disk before the Messages object is gone.
    //
    // worker_was_running_ records whether the background flush
    // thread was started in the constructor. If it wasn't (caller
    // disabled auto-flush), the destructor does NOT auto-save — the
    // caller is in full control of save() timing, and a destructive
    // "save on dtor" would surprise test code that wipes the data
    // dir in between Messages construction and destruction.
    mutable std::atomic<bool> dirty_{false};
    std::thread flush_worker_;
    std::condition_variable flush_cv_;
    std::mutex flush_mutex_;
    bool stop_flush_ = false;
    bool worker_was_running_ = false;

    // §CRASH-6 (2026-08-05): per-instance crash-mirror state (pre-opened
    // .crash fd + double-buffered payload), shared with the signal
    // handler. Allocated lazily by crash_state_or_create() on the first
    // save() or install_crash_handlers(), and INTENTIONALLY LEAKED for
    // the rest of the process: the signal handler may hold a pointer to
    // it at any moment, so freeing it (or closing the fd) would be a
    // use-after-free / stray-write hazard (monkeybyte report, defect
    // #4). mutable because save() is const.
    mutable MessagesCrashState* crash_state_ = nullptr;

    // §CRASH-6: lazily allocate crash_state_ (never freed).
    MessagesCrashState* crash_state_or_create() const;

    // Helper: flush worker thread entry point.
    void flush_worker_main();

    // §CRASH-5 (2026-07-16): helper for crash-file recovery.
    // load() tries this when messages.json is missing or corrupt.
    // load_from_crash_file() also runs the drop-last heuristic via
    // validate_message_for_recovery().
    bool load_from_crash_file(const std::string& crash_path);

    // §CRASH-5: returns true if `msg` is well-formed enough to keep
    // during crash recovery. Used by load_from_crash_file() to
    // decide whether to drop the last message.
    bool validate_message_for_recovery(const json& msg) const;

    // §CRASH-6 (2026-08-05, monkeybyte report defect #7): drop
    // structurally invalid messages from a freshly-loaded chain.
    // Returns the number dropped; used by load() on both the SQLite
    // mirror and the messages.json path.
    std::size_t drop_invalid_messages(json& arr, const std::string& source) const;
    
    // Curl handle for compression API calls
    
    // Helper: Find assistant message index with tool call
    int find_assistant_with_tool(const std::string& id) const;
    
    // Helper: Check if tool result exists
    bool has_tool_result(const std::string& id) const;
    
    // Helper: Check if tool result is an error
    bool is_error_result(const std::string& id) const;

    // §B13: shared helper — computes the safe split index (see split_messages).
    // Both split_messages and get_compression_indices use this to stay in sync.
    size_t compute_safe_split() const;

    // Helper: Split messages into prefix, middle, suffix
    void split_messages(json& prefix, json& middle, json& suffix) const;

    // §Phase 11: was on SubAgentCompressor (named
    // build_compression_prompt_for_subagent). Moved into Messages
    // because the only caller is Messages::compress_messages.
    // Synchronous compression path — no subprocess.
    static std::string build_compression_prompt(const std::string& middle_plain_text);
    
    // Helper: Call LLM for summary
    std::string call_llm_for_summary(const std::string& prompt);
    
    // Helper: Extract summary text from LLM response
    std::string extract_summary_text(const std::string& response_body);
    
    // Helper: Token estimation
    size_t estimate_tokens_from_json(const json& j) const;
    
    // Helper: Write debug file
    void write_debug_file(const std::string& filename, const std::string& content);
};
