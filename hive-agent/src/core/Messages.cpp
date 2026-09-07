/**
 * @file Messages.cpp
 * @brief Implementation of unified message handling and compression
 * 
 * ARCHITECTURE: Single Source of Truth
 * Tool call state is DERIVED from the message chain, not stored separately:
 * - PENDING: assistant message has tool_calls[X].id="abc", no tool result exists
 * - EXECUTING: runtime marker only (ephemeral, not persisted) - stored in executing_tools_
 * - COMPLETED: role="tool",tool_call_id="abc" exists with non-error content
 * - FAILED: role="tool",tool_call_id="abc" exists with error content
 * - INVALID: assistant message has tool_calls with malformed JSON arguments
 */

#include "Messages.h"
#include "core/settings.h"
#include <algorithm>
#include "api/HttpSession.h"
#include "Utf8Utils.h"
#include "Logger.h"

#include <fstream>
#include <sstream>
#include <iostream>
#include <cstdio>      // fileno(), ::fdopen()
#include <unistd.h>    // fileno(), fsync(), ::dup()
#include <fcntl.h>     // open(), O_CREAT, O_WRONLY, O_TRUNC
#include <sys/stat.h>  // mode_t
#include <csignal>     // sigaction
#include <vector>
#include <array>       // fixed-size crash registry
#include <cstdlib>     // malloc/free (crash payload slots)
#include <iterator>    // istreambuf_iterator
#include <cstring>     // strlen
#include <mutex>       // std::call_once
#include <algorithm>   // std::find, std::swap

// §Phase 5.1: helper macro that converts LOG_CERR(INFO,  ...); style
// statements into LOG_INFO/LOG_WARNING/LOG_ERROR via the existing
// Logger. Same formatted text routed through the Logger instead of
// writing to stderr directly.
#define LOG_CERR(level, expr) do {                          \
        std::ostringstream _daedalus_msg;                   \
        _daedalus_msg << expr;                               \
        LOG_##level(_daedalus_msg.str());                    \
    } while (0)
#include <thread>
#include <chrono>

// SANITIZE-NOTHROW: defensive string extraction (defined below, before
// sanitize_message_chain). Forward-declared so the earlier
// cleanup_dangling_tool_results / truncate_if_needed paths can use it
// too (§PERSIST-RACE hardening, 2026-09-02).
namespace { std::string chain_safe_str(const json& j, const char* key); }

// Helper function to sanitize strings to valid UTF-8
// Simple replacement-based approach (doesn't need iconv)
// ============================================================================
// Helper: Convert JSON messages to plain text for compression
// ============================================================================

// ============================================================================
// Helper: Convert JSON messages to plain text for compression
// ============================================================================

/**
 * @brief Convert a JSON message array to plain text format
 * 
 * Converts: [{"role":"user","content":"..."}, {"role":"assistant","content":"..."}]
 * To: "User: ...\nAssistant: ...\n"
 * 
 * This removes JSON overhead (~50% reduction in size) allowing larger
 * conversations to be compressed via sub-agent.
 */
std::string messages_to_plain_text(const json& messages) {
    std::ostringstream oss;
    for (const auto& msg : messages) {
        std::string role = msg.value("role", "unknown");

        // Handle content - can be string (normal) or array (multimodal)
        std::string content_str;
        if (msg.contains("content")) {
            const auto& content_val = msg["content"];
            if (content_val.is_string()) {
                content_str = content_val.get<std::string>();
            } else if (content_val.is_array()) {
                // Multimodal content - extract text parts
                for (const auto& item : content_val) {
                    if (item.is_object()) {
                        std::string item_type = item.value("type", "");
                        if (item_type == "text") {
                            std::string text = item.value("text", "");
                            if (!content_str.empty()) content_str += "\n";
                            content_str += text;
                        } else if (item_type == "image_url") {
                            // Note image presence for context
                            if (!content_str.empty()) content_str += "\n";
                            content_str += "[image attached]";
                        }
                    }
                }
                // If array had no extractable text, note that
                if (content_str.empty()) {
                    content_str = "[multimodal content]";
                }
            } else {
                // Object or other - serialize as fallback
                content_str = content_val.dump();
            }
        }

        // Capitalize role for readability
        if (!role.empty()) {
            role[0] = std::toupper(role[0]);
        }

        oss << role << ": " << content_str << "\n\n";
    }
    return oss.str();
}

// ============================================================================
// Compression helpers
// ============================================================================

// §Phase 11: was SubAgentCompressor::build_compression_prompt_for_subagent.
// Renamed + moved into Messages as a private static. The function has
// nothing to do with sub-agents anymore — it's a prompt template used
// by the synchronous compression path (Messages::compress_messages →
// Messages::call_llm_for_summary).
std::string Messages::build_compression_prompt(const std::string& middle_plain_text) {
    std::ostringstream oss;
    oss << R"(
You are a compression assistant for a coding agent conversation.

Your job is to SUMMARIZE the given portion of a conversation. The given
portion contains the main work that was done. You need to compress it into
a concise summary.

GIVEN CONVERSATION TO SUMMARIZE:
)" << middle_plain_text << R"(

Requirements:
- Maximum 500 words (~1500 tokens output)
- Focus on: key decisions made, files modified, commands run, errors encountered
- Be concise - this is a transitional summary to bridge prefix and suffix
- Output ONLY the summary text, nothing else

Format your output as:
KEY EVENTS: [bullets of what happened]
FILES: [list of files modified]
NEXT: [what needs to happen next]

Start your response with "KEY EVENTS:" and end with "NEXT:" - do not include any
prefix, suffix, or explanation. Just output the summary in the format above.
)";
    return oss.str();
}
// Phase 11: deleted SubAgentCompressor::wait_for_subagent_result() and
// SubAgentCompressor::compress_via_subagent() — both spawned
// 'hive-brain --sub-agent' subprocesses to do compression. That CLI flag
// was removed in Phase 2.7. The synchronous compression path
// (call_llm_for_summary below) is the only path used in practice.
// Constructor / Destructor
// ============================================================================

// ============================================================================
// §CRASH-4 (2026-07-16): async-signal-safe crash handler infrastructure.
// §CRASH-6 (2026-08-05): reworked per the monkeybyte crash report
// (hive-agent-bugs.md, defects #1-#5). The old design kept a
// std::vector<Messages*> guarded by a spinlock, snapshotted it into a
// std::vector inside the signal handler (malloc in a SIGSEGV handler —
// deadlock/heap corruption if the fault originated in the allocator),
// and let the handler dereference Messages objects that a concurrent
// destructor could free. The new design:
//
//   - registry is a FIXED-SIZE array of atomic pointers — no lock,
//     no dynamic storage anywhere on the signal path (defects #1/#3);
//   - per-instance state lives in a heap-allocated MessagesCrashState
//     that is INTENTIONALLY LEAKED for the rest of the process, so the
//     handler can never touch freed memory or a recycled fd
//     (defect #4);
//   - the payload is double-buffered: save() writes the inactive slot
//     and flips an atomic index, so the handler never reads a buffer
//     that save() is mutating (defect #2);
//   - after writing, the handler ftruncate()s to the payload length to
//     cut any stale tail (defect #5).
// ============================================================================

// Crash-mirror state shared between a Messages instance and the signal
// handler. Allocated lazily (first save() or install_crash_handlers())
// and deliberately never freed — see uninstall_crash_handlers().
struct MessagesCrashState {
    // Pre-opened fd for messages.json.crash. -1 until
    // install_crash_handlers() opens it. Never closed: closing it would
    // let the fd number be recycled while a concurrent signal handler
    // still writes through it (report defect #4).
    std::atomic<int> fd{-1};

    // false after uninstall_crash_handlers(); the handler skips
    // inactive entries.
    std::atomic<bool> active{true};

    // Bookkeeping for install/uninstall idempotency (normal threads).
    std::atomic<bool> registered{false};

    // Double-buffered payload (report defect #2). save() writes the
    // INACTIVE slot then flips active_slot with release ordering; the
    // handler loads active_slot once with acquire ordering and reads
    // that slot, so it never reads a buffer save() is mutating.
    struct PayloadSlot {
        char* data = nullptr;       // malloc'd, `capacity` bytes
        std::size_t size = 0;       // valid bytes in `data`
        std::size_t capacity = 0;
    };
    PayloadSlot slots[2];
    std::atomic<int> active_slot{0};
};

namespace {

// Lock-free fixed-size registry of crash-mirror states that want their
// .crash file written on SIGSEGV/SIGABRT/SIGFPE/SIGILL. Registration is
// a CAS on an empty slot — no lock is ever taken, so a thread that
// faults mid-registration cannot deadlock the handler (defect #3).
constexpr std::size_t kCrashRegistryCapacity = 64;
std::array<std::atomic<MessagesCrashState*>, kCrashRegistryCapacity>
    g_crash_registry{};

// Async-signal-safe write helper. Writes len bytes from buf to fd,
// looping on EINTR. Returns 0 on success, -1 on hard error. No
// std::cerr / printf — only write(2).
ssize_t signal_safe_write(int fd, const void* buf, std::size_t len) {
    const char* p = static_cast<const char*>(buf);
    std::size_t total = 0;
    while (total < len) {
        ssize_t n = ::write(fd, p + total, len - total);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (n == 0) return -1;
        total += static_cast<std::size_t>(n);
    }
    return static_cast<ssize_t>(total);
}

// Re-raise the signal with the original disposition so the user
// still gets their core dump / stack trace.
void re_raise(int sig) {
    ::signal(sig, SIG_DFL);
    ::sigset_t mask;
    ::sigemptyset(&mask);
    ::sigaddset(&mask, sig);
    ::sigprocmask(SIG_UNBLOCK, &mask, nullptr);
    ::raise(sig);
}

// The actual signal handler. Iterates the registry, writes each
// .crash file with the most recently persisted contents, then
// re-raises.
extern "C" void daedalus_crash_signal_handler(int sig) {
    // §CRASH-6: no allocation, no locks, no stdlib — only atomic loads
    // and async-signal-safe syscalls. The registry slots point at
    // leaked MessagesCrashState objects, so even a Messages destructor
    // racing the handler cannot produce a use-after-free.
    for (const auto& slot : g_crash_registry) {
        MessagesCrashState* st = slot.load(std::memory_order_acquire);
        if (!st) continue;
        if (!st->active.load(std::memory_order_acquire)) continue;
        const int fd = st->fd.load(std::memory_order_relaxed);
        if (fd < 0) continue;
        const MessagesCrashState::PayloadSlot& s =
            st->slots[st->active_slot.load(std::memory_order_acquire) & 1];
        if (!s.data || s.size == 0) continue;
        ::lseek(fd, 0, SEEK_SET);
        if (signal_safe_write(fd, s.data, s.size) >= 0) {
            // ftruncate(2) is async-signal-safe. Cutting the file at
            // exactly the payload length keeps the mirror a valid JSON
            // document even when the new payload is shorter than the
            // previous one (report defect #5). Best-effort: if the
            // truncate fails, the recovery-side parser tolerates the
            // stale tail (see load_from_crash_file()).
            const int trunc_rc = ::ftruncate(fd, static_cast<off_t>(s.size));
            (void)trunc_rc;
        }
        ::fsync(fd);
    }
    re_raise(sig);
}

// Registry add/remove — normal-thread context only.
bool crash_registry_add(MessagesCrashState* st) {
    for (auto& slot : g_crash_registry) {
        MessagesCrashState* expected = nullptr;
        if (slot.compare_exchange_strong(expected, st,
                                         std::memory_order_release,
                                         std::memory_order_relaxed)) {
            return true;
        }
    }
    return false;
}

void crash_registry_remove(MessagesCrashState* st) {
    for (auto& slot : g_crash_registry) {
        MessagesCrashState* expected = st;
        if (slot.compare_exchange_strong(expected, nullptr,
                                         std::memory_order_release,
                                         std::memory_order_relaxed)) {
            return;
        }
    }
}

// Publish a new payload: write the inactive slot, then flip the atomic
// index. Called from save() (normal context — malloc is fine here).
void crash_state_set_payload(MessagesCrashState* st,
                             const char* data, std::size_t size) {
    const int next = 1 - st->active_slot.load(std::memory_order_relaxed);
    auto& s = st->slots[next];
    if (s.capacity < size) {
        char* nb = static_cast<char*>(std::malloc(size > 0 ? size : 1));
        if (!nb) return;  // OOM: keep the previous payload
        std::free(s.data);
        s.data = nb;
        s.capacity = size;
    }
    if (size > 0) std::memcpy(s.data, data, size);
    s.size = size;
    st->active_slot.store(next, std::memory_order_release);
}

// One-time install of the signal handler for SIGSEGV/SIGABRT/SIGFPE/SIGILL.
void install_signal_handlers_once() {
    static std::atomic<bool> installed{false};
    if (installed.exchange(true)) return;
    struct sigaction sa{};
    sa.sa_handler = daedalus_crash_signal_handler;
    sigemptyset(&sa.sa_mask);
    // SA_NODEFER: don't block the signal we're handling (so re-raise
    // works). SA_RESETHAND: revert to SIG_DFL after one delivery, so
    // a recursive crash still produces a core dump.
    sa.sa_flags = SA_NODEFER | SA_RESETHAND;
    ::sigaction(SIGSEGV, &sa, nullptr);
    ::sigaction(SIGABRT, &sa, nullptr);
    ::sigaction(SIGFPE,  &sa, nullptr);
    ::sigaction(SIGILL,  &sa, nullptr);
}

}  // namespace

Messages::Messages(std::filesystem::path data_dir,
                 const MessageCompressionConfig& config)
    : data_dir_(std::move(data_dir))
    , messages_(json::array())
    , config_(config) {

    // §Phase 8.8 / §Phase 16: data_dir is the fully-resolved namespace
    // directory, passed in by AgentImpl::initialize() via
    // resolve_agent_dir(project_root, agent_name). The default
    // (empty agent_name) yields `<project>/.daedalus/hive-brain/`.
    messages_file_ = data_dir_ / "messages.json";

    // Ensure the namespace directory exists.
    if (!std::filesystem::exists(data_dir_)) {
        std::filesystem::create_directories(data_dir_);
    }

    // §CRASH-2 (2026-07-16): start the write-through flush worker.
    // The worker waits on flush_cv_; when an add_* sets dirty_ and
    // notifies, it wakes up and calls save(). On destruction,
    // stop_flush_ is set, the worker wakes, does one final flush,
    // and exits.
    //
    // Tests can opt out via config.auto_flush_enabled = false if
    // they want full control over save() timing. We remember this
    // choice in worker_was_running_ so the destructor knows whether
    // to flush final dirty state.
    worker_was_running_ = config_.auto_flush_enabled;
    if (config_.auto_flush_enabled) {
        flush_worker_ = std::thread([this]{ flush_worker_main(); });
    }
}

Messages::~Messages() {
    // §CRASH-2 (2026-07-16): ask the flush worker to stop and join.
    // Any final dirty state is flushed by the worker before exit, so
    // messages added in the destructor or just before it are not lost.
    if (flush_worker_.joinable()) {
        {
            std::lock_guard<std::mutex> lk(flush_mutex_);
            stop_flush_ = true;
        }
        flush_cv_.notify_all();
        flush_worker_.join();
    }
    // Note: we deliberately do NOT auto-save in the destructor if
    // the worker was disabled. The caller opted out of background
    // flushing and is responsible for the save() timing. A surprise
    // save() here would re-create the messages file even after the
    // test wiped the data dir (destroying the test isolation).

    // §CRASH-4 (2026-07-16): deregister from the signal-handler
    // registry. Callers should have called uninstall_crash_handlers()
    // in the graceful shutdown path; we also do it here defensively so
    // the handler stops writing this instance's .crash file. (The
    // crash-mirror state itself is intentionally leaked — see
    // uninstall_crash_handlers.)
    uninstall_crash_handlers();
}

// §CRASH-2 (2026-07-16): move ops are deleted in the header
// (atomic + thread are not movable). RVO handles return-by-value.

// ============================================================================
// Message Access
// ============================================================================

std::string Messages::get_system_content() const {
    std::lock_guard<std::mutex> lk(messages_mutex_);
    if (messages_.empty()) return "";
    if (messages_[0].value("role", "") == "system") {
        return messages_[0].value("content", "");
    }
    return "";
}

std::string Messages::get_last_assistant_content() const {
    std::lock_guard<std::mutex> lk(messages_mutex_);
    for (int i = static_cast<int>(messages_.size()) - 1; i >= 0; --i) {
        if (messages_[i].value("role", "") == "assistant") {
            return messages_[i].value("content", "");
        }
    }
    return "";
}

// ============================================================================
// Message Operations
// ============================================================================

void Messages::add_system(const std::string& content) {
    {
        std::lock_guard<std::mutex> lk(messages_mutex_);
        if (!messages_.empty()) {
            LOG_CERR(INFO,  "[Messages] Warning: System message already exists, not adding" << '\n');
            return;
        }
        messages_.push_back({{"role", "system"}, {"content", content}});
        mark_dirty_locked();  // §PERSIST-RACE: publish snapshot at mark time
    }
    // §CRASH-2 (2026-07-16): notify the flush worker (dirty set under lock).
    if (config_.auto_flush_enabled) {
        flush_cv_.notify_all();
    }
}

void Messages::add_user(const std::string& content, const std::string& image_url) {
    json msg;
    msg["role"] = "user";

    if (!image_url.empty()) {
        // Multi-modal: array content with text and image
        json content_array = json::array();
        content_array.push_back({{"type", "text"}, {"text", content}});
        content_array.push_back({{"type", "image_url"}, {"image_url", {{"url", image_url}}}});
        msg["content"] = content_array;
    } else {
        msg["content"] = content;
    }

    {
        std::lock_guard<std::mutex> lk(messages_mutex_);
        messages_.push_back(msg);
        mark_dirty_locked();  // §PERSIST-RACE: publish snapshot at mark time
    }
    // §CRASH-2 (2026-07-16): notify the flush worker (dirty set under lock).
    if (config_.auto_flush_enabled) {
        flush_cv_.notify_all();
    }
}

void Messages::add_assistant(const std::string& content) {
    {
        std::lock_guard<std::mutex> lk(messages_mutex_);
        messages_.push_back({{"role", "assistant"}, {"content", content}});
        mark_dirty_locked();  // §PERSIST-RACE: publish snapshot at mark time
    }
    // §CRASH-2 (2026-07-16): notify the flush worker (dirty set under lock).
    if (config_.auto_flush_enabled) {
        flush_cv_.notify_all();
    }
}

void Messages::add_tool_result(const std::string& tool_call_id, const std::string& content, bool is_error) {
    // Auto-detect error if not explicitly specified and content starts with "Error:"
    if (!is_error && content.find("Error:") == 0) {
        is_error = true;
    }
    {
        std::lock_guard<std::mutex> lk(messages_mutex_);
        messages_.push_back({
            {"role", "tool"},
            {"tool_call_id", tool_call_id},
            {"content", content},
            {"status", is_error ? "error" : "success"}
        });
        mark_dirty_locked();  // §PERSIST-RACE: publish snapshot at mark time
    }
    // §CRASH-2 (2026-07-16): notify the flush worker (dirty set under lock).
    if (config_.auto_flush_enabled) {
        flush_cv_.notify_all();
    }
}

void Messages::add_message(const json& msg) {
    {
        std::lock_guard<std::mutex> lk(messages_mutex_);
        messages_.push_back(msg);
        mark_dirty_locked();  // §PERSIST-RACE: publish snapshot at mark time
    }
    // §CRASH-2 (2026-07-16): notify the flush worker (dirty set under lock).
    if (config_.auto_flush_enabled) {
        flush_cv_.notify_all();
    }
}

void Messages::set_messages(const json& msgs) {
    {
        std::lock_guard<std::mutex> lk(messages_mutex_);
        messages_ = msgs;
        // PERSIST-DIRTY (2026-08-31): set_messages() rewrites the chain
        // (sanitize/truncate/compress paths in LLMClient) - it must mark
        // dirty like add_*() does, otherwise the rewrite never reaches
        // disk. §PERSIST-RACE: publish in the same critical section.
        mark_dirty_locked();
    }
    if (config_.auto_flush_enabled) {
        flush_cv_.notify_all();
    }
}

// PERSIST-DIRTY (2026-08-31): see header. The LLMClient turn path
// mutates the chain through the raw get_messages() reference; this lets
// it rejoin the write-through persistence design (CRASH-2) without
// rerouting every push through add_*().
// §PERSIST-RACE (2026-09-02): caller must hold messages_mutex_.
void Messages::mark_dirty_locked() {
    persist_snapshot_ = messages_;
    dirty_.store(true, std::memory_order_release);
}

void Messages::mark_dirty() {
    {
        std::lock_guard<std::mutex> lk(messages_mutex_);
        mark_dirty_locked();
    }
    if (config_.auto_flush_enabled) {
        flush_cv_.notify_all();
    }
}

// ============================================================================
// Persistence
// ============================================================================

bool Messages::load() {
    // §Plan.md v0.3.5: try SQLite mirror first. Fall back to JSON file.
    // §B22 fix: when the SQLite mirror is corrupt (parse error) or empty,
    // delete it so the next load doesn't try it again.
    try {
        auto store = hive::SettingsStore::for_home(
            messages_file_.parent_path());
        auto blob = store.get_messages_json(session_id_);
        if (!blob.empty()) {
            try {
                json saved = json::parse(blob);
                if (saved.is_array() && !saved.empty()) {
                    // §CRASH-6 (defect #7): drop structurally invalid
                    // entries instead of feeding them to the provider.
                    drop_invalid_messages(saved, "SQLite mirror");
                    if (saved.empty()) {
                        // Everything was invalid — the mirror is poison.
                        LOG_WARNING("Messages: SQLite mirror held only "
                                    "invalid messages; deleting and "
                                    "falling back to JSON");
                        store.delete_messages_json(session_id_);
                    } else {
                        std::lock_guard<std::mutex> lk(messages_mutex_);
                        messages_ = saved;
                        messages_ = sanitize_message_chain(messages_);
                        LOG_INFO("Loaded " + std::to_string(messages_.size())
                                 + " messages from SQLite mirror");
                        return true;
                    }
                }
            } catch (const json::parse_error&) {
                // Corrupt mirror: delete it and fall through to JSON load (B22)
                LOG_WARNING("Messages: SQLite mirror corrupt; deleting and falling back to JSON");
                store.delete_messages_json(session_id_);
            }
        }
    } catch (const std::exception& e) {
        LOG_WARNING("Messages: SQLite mirror load failed ("
                    + std::string(e.what()) + "); falling back to JSON");
    }

    if (!std::filesystem::exists(messages_file_)) {
        // §CRASH-5 (2026-07-16): try the .crash mirror as a fallback.
        // The crash handler (Phase 4) wrote this on SIGSEGV/SIGABRT/etc.
        // It may be the only copy of the messages if the process died
        // before save() finished writing messages.json.
        const std::string crash_path = messages_file_.string() + ".crash";
        if (load_from_crash_file(crash_path)) {
            return true;
        }
        return false;
    }

    try {
        std::ifstream f(messages_file_);
        if (!f.good()) {
            LOG_WARNING("Messages file exists but cannot be read: " + messages_file_.string());
            return false;
        }

        json saved = json::parse(f);
        if (!saved.is_array()) {
            LOG_WARNING("Messages file contains invalid format (not an array)");
            return false;
        }

        if (saved.empty()) {
            LOG_INFO("Messages file is empty");
            return false;
        }

        // §CRASH-6 (defect #7): drop structurally invalid entries
        // instead of feeding them to the provider.
        drop_invalid_messages(saved, messages_file_.string());
        if (saved.empty()) {
            LOG_WARNING("Messages file contained only invalid messages; "
                        "starting fresh");
            return false;
        }

        {
            std::lock_guard<std::mutex> lk(messages_mutex_);
            messages_ = saved;

            // Always sanitize on load to remove orphaned tool results
            messages_ = sanitize_message_chain(messages_);
        }

        LOG_INFO("Loaded " + std::to_string(messages_.size()) + " messages from " + messages_file_.string());
        return true;

    } catch (const json::parse_error& e) {
        // §CRASH-5 (2026-07-16): JSON in the main file is corrupt —
        // it may have been a half-written state from a previous crash.
        // Try the .crash mirror before giving up.
        LOG_WARNING("JSON parse error in messages file ("
                    + std::string(e.what()) + "); trying .crash mirror");
        const std::string crash_path = messages_file_.string() + ".crash";
        if (load_from_crash_file(crash_path)) {
            return true;
        }
        return false;
    } catch (const std::exception& e) {
        LOG_ERROR("Failed to load messages: " + std::string(e.what()));
        return false;
    }
}

// §CRASH-5 (2026-07-16): helper to load from messages.json.crash.
//
// Called from load() when the main messages.json is missing or
// corrupt. The .crash file is the same JSON array format, written
// by the crash handler at the last successfully-saved state.
//
// After loading, we run a defensive "drop last message" heuristic
// (Phase 5): if the last message in the chain fails a basic
// validity check (no role, missing tool_call_id, etc.) AND the
// second-to-last is well-formed, drop the last message and re-save
// the chain. This covers the common case where a malformed tool
// result caused a parser crash mid-add.
bool Messages::load_from_crash_file(const std::string& crash_path) {
    if (!std::filesystem::exists(crash_path)) return false;

    try {
        std::ifstream f(crash_path);
        if (!f.good()) {
            LOG_WARNING("Crash file exists but cannot be read: " + crash_path);
            return false;
        }
        // §CRASH-6 (report defect #5): the .crash mirror is written
        // through a pre-opened fd; if a truncation ever failed the file
        // can carry a stale tail after the closing ']'. Tolerate that:
        // walk backwards over ']' candidates and take the first prefix
        // that parses as a valid document.
        const std::string text{std::istreambuf_iterator<char>(f),
                               std::istreambuf_iterator<char>()};
        json saved;
        try {
            saved = json::parse(text);
        } catch (const json::parse_error&) {
            bool recovered = false;
            std::size_t pos = text.find_last_of(']');
            for (int attempts = 0;
                 !recovered && attempts < 16 && pos != std::string::npos;
                 ++attempts) {
                try {
                    saved = json::parse(text.substr(0, pos + 1));
                    recovered = true;
                } catch (const json::parse_error&) {
                    pos = (pos == 0) ? std::string::npos
                                     : text.find_last_of(']', pos - 1);
                }
            }
            if (!recovered) {
                throw;  // rethrow the original parse error
            }
            LOG_WARNING("Messages: .crash file had trailing bytes; "
                        "parsed up to the last valid ']'");
        }
        if (!saved.is_array() || saved.empty()) {
            LOG_WARNING("Crash file is empty or not an array: " + crash_path);
            return false;
        }

        // §CRASH-5: drop-last heuristic. If the last message is
        // suspect (missing role, missing tool_call_id where required,
        // unparseable inner JSON) AND the second-to-last is fine,
        // drop the last. We only drop ONE message — if more are
        // corrupt, the user gets them all back to inspect.
        if (saved.size() >= 2 && !validate_message_for_recovery(saved.back())) {
            const json& second_to_last = saved[saved.size() - 2];
            if (validate_message_for_recovery(second_to_last)) {
                LOG_WARNING("Messages: dropping suspect last message from .crash recovery: "
                            + saved.back().dump());
                saved.erase(saved.end() - 1);
                // Re-save the recovered chain to messages.json so the
                // next launch goes through the normal path.
                // We don't write .crash again here — install_crash_handlers
                // (if used) will re-create it on the next save().
                try {
                    std::filesystem::create_directories(messages_file_.parent_path());
                    const std::string tmp = messages_file_.string() + ".tmp";
                    std::FILE* wf = std::fopen(tmp.c_str(), "wb");
                    if (wf) {
                        const std::string blob = saved.dump(2);
                        std::fwrite(blob.data(), 1, blob.size(), wf);
                        std::fflush(wf);
#ifndef _WIN32
                        ::fsync(::fileno(wf));
#endif
                        std::fclose(wf);
                        std::filesystem::rename(tmp, messages_file_);
                        LOG_INFO("Messages: re-saved recovered chain to " + messages_file_.string());
                    }
                } catch (...) { /* best-effort */ }
            }
        }

        {
            std::lock_guard<std::mutex> lk(messages_mutex_);
            messages_ = saved;
            messages_ = sanitize_message_chain(messages_);
        }
        LOG_INFO("Loaded " + std::to_string(messages_.size()) + " messages from .crash mirror " + crash_path);
        return true;
    } catch (const json::parse_error& e) {
        LOG_WARNING("JSON parse error in .crash file: " + std::string(e.what()));
        return false;
    } catch (const std::exception& e) {
        LOG_WARNING("Failed to load .crash file: " + std::string(e.what()));
        return false;
    }
}

// §CRASH-5 (2026-07-16): validate a single message for crash recovery.
// Returns true if the message is well-formed enough to keep; false
// if it's likely the cause of a crash (malformed JSON, missing role,
// tool result missing tool_call_id, etc.).
bool Messages::validate_message_for_recovery(const json& msg) const {
    if (!msg.is_object()) return false;
    if (!msg.contains("role") || !msg["role"].is_string()) return false;
    if (msg["role"].get<std::string>().empty()) return false;

    // Tool results must reference a tool_call_id.
    if (msg["role"] == "tool") {
        if (!msg.contains("tool_call_id") ||
            !msg["tool_call_id"].is_string() ||
            msg["tool_call_id"].get<std::string>().empty()) {
            return false;
        }
    }
    // Assistant messages with tool_calls must have valid JSON
    // arguments in each call (a malformed argument string is a
    // classic cause of "exception while parsing tool call" crashes).
    if (msg["role"] == "assistant" && msg.contains("tool_calls")
        && msg["tool_calls"].is_array()) {
        for (const auto& tc : msg["tool_calls"]) {
            if (!tc.is_object()) return false;
            if (!tc.contains("id") || !tc["id"].is_string()) return false;
            if (tc.contains("function") && tc["function"].is_object()) {
                const auto& fn = tc["function"];
                if (fn.contains("arguments")) {
                    if (fn["arguments"].is_string()) {
                        // Try to parse — if it's malformed JSON, drop.
                        try {
                            [[maybe_unused]] const auto parsed =
                                json::parse(fn["arguments"].get<std::string>());
                        } catch (...) {
                            return false;
                        }
                    } else if (!fn["arguments"].is_object()) {
                        // arguments must be a string or object, nothing else
                        return false;
                    }
                }
            }
        }
    }
    return true;
}

// §CRASH-6 (monkeybyte report, defect #7): validate every message of a
// loaded chain, not just the last one on the .crash recovery path.
// load() used to run only sanitize_message_chain() (which fixes
// orphaned tool results) — a message with a missing/empty role that
// landed in the store once (e.g. written by an interrupted save) was
// loaded forever after, and every LLM request built from the chain
// failed. Reuses the crash-recovery validator; invalid entries are
// dropped with a warning instead of fed back to the provider.
std::size_t Messages::drop_invalid_messages(json& arr,
                                            const std::string& source) const {
    std::size_t dropped = 0;
    json valid = json::array();
    for (const auto& m : arr) {
        if (validate_message_for_recovery(m)) {
            valid.push_back(m);
        } else {
            ++dropped;
            LOG_WARNING("Messages: dropping structurally invalid message "
                        "from " + source + ": " + m.dump());
        }
    }
    if (dropped > 0) {
        arr = std::move(valid);
    }
    return dropped;
}

void Messages::save() const {
    // §PERSIST-RACE: refresh the published snapshot first, so a caller
    // that mutated via the raw get_messages() reference without
    // mark_dirty() still persists the live chain (legacy save()
    // semantics). Only main-thread-style callers use save(); the flush
    // worker goes through flush_if_dirty(), which must NOT read
    // messages_ and therefore skips this refresh.
    {
        std::lock_guard<std::mutex> lk(messages_mutex_);
        persist_snapshot_ = messages_;
    }
    // §fix (2026-07-18): serialize concurrent save() calls — the flush
    // worker and a main-thread flush_now() can run save() at the same
    // time and would race on the crash-mirror state.
    std::lock_guard<std::mutex> save_lock(save_mutex_);
    save_impl();
}

// §fix (2026-07-18): unsynchronized persistence core. Caller must hold
// save_mutex_ so that crash-mirror mutations and the atomic
// dirty_/snapshot dance are serialized.
void Messages::save_impl() const {
    // §PERSIST-RACE (2026-09-02, hive-agent-bugs.md BUG 1): snapshot the
    // PUBLISHED chain, never messages_ itself. The LLMClient turn path
    // mutates messages_ through the raw get_messages() reference without
    // holding messages_mutex_; a deep copy racing those mutations is a
    // torn read (json object header with null payload -> SIGSEGV in the
    // flush worker, killing the host). persist_snapshot_ is written only
    // at mark time, on the mutating thread, at quiescent points.
    // Clear dirty_ atomically with the snapshot; it is re-set on failure
    // below so the worker retries on the next tick.
    json snapshot;
    {
        std::lock_guard<std::mutex> lk(messages_mutex_);
        snapshot = persist_snapshot_;
        dirty_.store(false, std::memory_order_release);
    }

    if (snapshot.empty()) {
        // Remove empty messages file
        if (std::filesystem::exists(messages_file_)) {
            std::filesystem::remove(messages_file_);
            LOG_DEBUG("Removed empty messages file");
        }
        // §CRASH-1 (2026-07-16): also remove any stale .tmp or .crash
        // file left from a previous incomplete save. Otherwise an
        // empty session would leave a .crash file behind that the
        // next load() could mistake for a recoverable session.
        try {
            std::filesystem::remove(messages_file_.string() + ".tmp");
            std::filesystem::remove(messages_file_.string() + ".crash");
        } catch (...) { /* best-effort */ }
        // §fix (2026-07-18): also delete the SQLite mirror. load()
        // consults the mirror FIRST, so without this a cleared history
        // would be resurrected from the mirror on the next session.
        try {
            auto store = hive::SettingsStore::for_home(
                messages_file_.parent_path());
            store.delete_messages_json(session_id_);
        } catch (...) { /* best-effort */ }
        // §CRASH-6 (report defect #6): clear the crash payload too —
        // otherwise the signal handler would rewrite the stale chain
        // into the .crash mirror, resurrecting a cleared session on the
        // next load().
        if (crash_state_) {
            crash_state_set_payload(crash_state_, "", 0);
        }
        return;
    }

    // Sanitize before saving - remove orphaned tool results.
    // SANITIZE-NOTHROW: last-resort catch -- any persistence error that
    // escapes here would ride the flush worker thread into
    // std::terminate (SIGABRT). Re-mark dirty so the next tick retries.
    json to_save;
    try {
        to_save = sanitize_message_chain(snapshot);
    } catch (const std::exception& e) {
        LOG_CERR(ERROR, "[Messages] sanitize_message_chain threw during save: " << e.what() << '\n');
        dirty_.store(true, std::memory_order_release);
        return;
    } catch (...) {
        LOG_CERR(ERROR, "[Messages] sanitize_message_chain threw unknown during save" << '\n');
        dirty_.store(true, std::memory_order_release);
        return;
    }

    // §CRASH-4 (2026-07-16): declare `contents` at function scope so
    // it's visible after the try block where the crash-mirror state is
    // updated.
    // §BUG-UTF8-SAVE (2026-09-02): a tool result carrying invalid UTF-8
    // (binary bash output etc.) made dump() throw type_error.316 here;
    // on the flush worker that was std::terminate -> SIGABRT even with
    // sanitize hardened. Replace invalid sequences like the LLMClient
    // payload path does (§BUG-UTF8-TRUNC).
    const std::string contents = to_save.dump(2, ' ', false,
                                              json::error_handler_t::replace);

    try {
        // §Phase 8.8 / §Phase 16: parent_path() is the per-agent
        // namespace dir (e.g. `.daedalus/hive-brain/` or
        // `.daedalus/daedalus-bob/`), already created in the ctor. We
        // only need to ensure it still exists at save time in case the
        // user moved/deleted it between sessions.
        std::filesystem::path daedalus_dir = messages_file_.parent_path();
        if (!std::filesystem::exists(daedalus_dir)) {
            std::filesystem::create_directories(daedalus_dir);
        }

        // §CRASH-1 (2026-07-16): atomic write via temp file + rename.
        //
        // Problem: the old code wrote directly to messages_file_ with
        // std::ofstream. A kill -9 or power loss mid-write left a
        // truncated file on disk; the next load() would parse-error
        // and silently drop to an empty session, losing every message.
        //
        // Fix: write to a sibling .tmp file, fsync, close, then
        // `rename(tmp, final)`. POSIX rename() on the same filesystem
        // is atomic — readers always see either the old file or the
        // new file, never a half-written one.
        //
        // We re-open the file via the C API in write_atomic() so we
        // get a portable fd for fsync(2). std::ofstream has no
        // portable native_handle() in C++23.
        const std::string tmp_path   = messages_file_.string() + ".tmp";
        const std::string crash_path = messages_file_.string() + ".crash";

        auto write_atomic = [&](const std::string& path) -> bool {
            // C API for portable fd access.
            std::FILE* wf = std::fopen(path.c_str(), "wb");
            if (!wf) {
                LOG_ERROR("Cannot open for writing: " + path);
                return false;
            }
            const std::size_t wrote = std::fwrite(contents.data(), 1,
                                                  contents.size(), wf);
            if (wrote != contents.size()) {
                LOG_ERROR("Short write to " + path + " ("
                          + std::to_string(wrote) + "/"
                          + std::to_string(contents.size()) + " bytes)");
                std::fclose(wf);
                return false;
            }
            std::fflush(wf);
#ifndef _WIN32
            // fsync the file data. Without this, a power loss could
            // leave the rename() ahead of the data write, producing
            // an empty target file.
            ::fsync(::fileno(wf));
#endif
            std::fclose(wf);
            return true;
        };

        if (!write_atomic(tmp_path)) {
            return;
        }
        // Atomic rename. Throws on filesystem error (cross-device,
        // permission denied, etc.).
        std::filesystem::rename(tmp_path, messages_file_);

        LOG_INFO("Saved " + std::to_string(to_save.size()) + " messages to " + messages_file_.string());

        // §CRASH-1: also write the async-signal-safe crash mirror.
        // The crash handler (Phase 4) will re-write this on signal,
        // but keeping it in sync on every save means we always have
        // a recoverable copy at most one save() behind.
        try {
            write_atomic(crash_path);
        } catch (const std::exception& e) {
            LOG_WARNING("Messages: .crash mirror write failed ("
                        + std::string(e.what()) + ")");
        }

    } catch (const std::exception& e) {
        LOG_ERROR("Failed to save messages: " + std::string(e.what()));
        // §fix: dirty_ was already cleared at snapshot time; re-set it
        // so the worker tries again on the next tick.
        dirty_.store(true, std::memory_order_release);
        return;
    }

    // §Plan.md v0.3.5: write-through mirror to SQLite (best-effort).
    // The empty case is handled above (mirror deleted); here the
    // snapshot is always non-empty.
    try {
        json mirror = sanitize_message_chain(snapshot);
        auto store = hive::SettingsStore::for_home(
            messages_file_.parent_path());
        store.set_messages_json(session_id_, mirror.dump());
    } catch (const std::exception& e) {
        LOG_WARNING("Messages: SQLite mirror save failed ("
                    + std::string(e.what()) + ")");
    }

    // §fix: dirty_ stays cleared (done at snapshot time). A concurrent
    // add_*() after the snapshot re-sets it, so nothing is lost.

    // §CRASH-4 (2026-07-16) / §CRASH-6 (2026-08-05): publish the JSON
    // payload into the double-buffered crash state so the signal handler
    // can re-write it verbatim without allocating or racing save()
    // (report defect #2), and keep the pre-opened .crash fd in sync:
    // write, then ftruncate to the exact payload length to cut any
    // stale tail (report defect #5).
    MessagesCrashState* st = crash_state_or_create();
    crash_state_set_payload(st, contents.data(), contents.size());
    const int cfd = st->fd.load(std::memory_order_acquire);
    if (cfd >= 0) {
        // Re-write the .crash file via the pre-opened fd so the
        // crash handler has the latest contents even if a save()
        // happened after the fd was opened.
        ::lseek(cfd, 0, SEEK_SET);
        if (signal_safe_write(cfd, contents.data(), contents.size()) >= 0) {
            if (::ftruncate(cfd, static_cast<off_t>(contents.size())) != 0) {
                LOG_WARNING("Messages: ftruncate on .crash mirror failed; "
                            "file may carry a stale tail");
            }
        }
        ::fsync(cfd);
        ::lseek(cfd, 0, SEEK_SET);
    }
}

// ============================================================================
// §CRASH-4 (2026-07-16): crash handler install / uninstall
// ============================================================================

MessagesCrashState* Messages::crash_state_or_create() const {
    // §CRASH-6: the state is allocated once and INTENTIONALLY LEAKED
    // (report defect #4). A signal can arrive at any moment; the handler
    // must never dereference freed memory or write through a recycled
    // fd, so the state — including the .crash fd — lives for the rest
    // of the process. Cost: one struct + one fd per Messages instance.
    if (!crash_state_) {
        crash_state_ = new MessagesCrashState();
    }
    return crash_state_;
}

std::string Messages::crash_payload_for_signal() const {
    // §CRASH-6: snapshot of the active payload slot. Called from normal
    // (non-signal) context — tests, diagnostics — so copying is fine.
    if (!crash_state_) return {};
    const auto& s = crash_state_->slots[
        crash_state_->active_slot.load(std::memory_order_acquire) & 1];
    if (!s.data || s.size == 0) return {};
    return std::string(s.data, s.size);
}

void Messages::install_crash_handlers() {
    MessagesCrashState* st = crash_state_or_create();
    // Open (or truncate) the .crash file once. The fd is held open for
    // the rest of the process (intentionally — see
    // uninstall_crash_handlers). The signal handler writes to this fd —
    // no allocation, no stdio.
    if (st->fd.load(std::memory_order_relaxed) < 0) {
        const std::string crash_path = messages_file_.string() + ".crash";
        // Ensure the directory exists (data_dir_ was created in the
        // ctor, but be defensive).
        std::error_code ec;
        std::filesystem::create_directories(messages_file_.parent_path(), ec);
        const int fd = ::open(crash_path.c_str(),
                              O_CREAT | O_WRONLY | O_TRUNC, 0644);
        if (fd >= 0) {
            // Write the current payload (empty if nothing saved yet) so
            // the fd has a valid byte sequence from the start.
            const auto& s = st->slots[
                st->active_slot.load(std::memory_order_acquire) & 1];
            if (s.data && s.size > 0) {
                signal_safe_write(fd, s.data, s.size);
                ::fsync(fd);
                ::lseek(fd, 0, SEEK_SET);
            }
            st->fd.store(fd, std::memory_order_release);
        } else {
            LOG_WARNING("Messages: cannot open .crash mirror: " + crash_path);
        }
    }
    // Install the signal handlers (one-time, process-global).
    install_signal_handlers_once();
    // Register ourselves in the process-global registry (lock-free CAS).
    if (!st->registered.load(std::memory_order_relaxed)) {
        st->active.store(true, std::memory_order_release);
        if (crash_registry_add(st)) {
            st->registered.store(true, std::memory_order_release);
        } else {
            LOG_ERROR("Messages: crash-mirror registry is full ("
                      + std::to_string(kCrashRegistryCapacity)
                      + " entries); this instance will not be written "
                        "on crash");
        }
    }
}

void Messages::uninstall_crash_handlers() {
    MessagesCrashState* st = crash_state_;
    if (!st) return;
    st->active.store(false, std::memory_order_release);
    if (st->registered.exchange(false, std::memory_order_acq_rel)) {
        crash_registry_remove(st);
    }
    // §CRASH-6 (report defect #4): neither the state nor the fd is
    // released here. A signal handler on another thread may already
    // have loaded `st` from the registry; freeing it — or closing the
    // fd, whose number could then be recycled by an unrelated open —
    // would turn that into a use-after-free or a stray write into an
    // unrelated file. Leaking one struct + one fd per Messages
    // instance is the deliberate crash-path trade-off. The .crash file
    // itself stays on disk for crash recovery on the next launch.
}

// ============================================================================
// §CRASH-2 (2026-07-16): write-through auto-flush
// ============================================================================

void Messages::flush_now() const {
    // Caller wants the data on disk *now* (e.g. before dispatching an
    // LLM call). We must wait for any in-progress background save() to
    // finish (dirty_ may already be cleared while the worker is still
    // writing/renaming the file), then flush any remaining dirty state.
    std::lock_guard<std::mutex> save_lock(save_mutex_);
    if (dirty_.load(std::memory_order_acquire)) {
        save_impl();
    }
}

bool Messages::flush_if_dirty() const {
    // Non-blocking. Used by the background flush worker.
    if (!dirty_.load(std::memory_order_acquire)) {
        return false;
    }
    // §PERSIST-RACE: the worker must never read messages_ (that read
    // races raw-reference turn-path mutations). save() refreshes the
    // snapshot from messages_, so the worker cannot use it - take
    // save_mutex_ and persist the snapshot published at mark time.
    std::lock_guard<std::mutex> save_lock(save_mutex_);
    save_impl();
    return true;
}

void Messages::flush_worker_main() {
    // Worker thread entry point. Loops on flush_cv_ with a timeout,
    // exits cleanly when stop_flush_ is set.
    //
    // Order of operations on stop:
    //   1. lock flush_mutex_
    //   2. set stop_flush_ = true
    //   3. notify_all (worker wakes from cv.wait_for)
    //   4. join
    // Worker sees stop_flush_, does a final flush_if_dirty(), and
    // returns. The mutex is released during the wait, so the worker
    // can re-acquire it to read stop_flush_.
    while (true) {
        std::unique_lock<std::mutex> lk(flush_mutex_);
        // Wait until either:
        //   - dirty_ is set (an add_* notified us), OR
        //   - the timeout elapses (safety net), OR
        //   - stop_flush_ is set (shutdown).
        flush_cv_.wait_for(
            lk,
            std::chrono::milliseconds(config_.auto_flush_interval_ms),
            [this] { return dirty_.load(std::memory_order_acquire)
                       || stop_flush_; });
        if (stop_flush_) {
            // Final flush so nothing in-flight is lost.
            lk.unlock();
            // SANITIZE-NOTHROW: this thread must never let an exception
            // escape -- std::thread + uncaught exception = terminate.
            try { flush_if_dirty(); }
            catch (const std::exception& e) {
                LOG_CERR(ERROR, "[Messages] flush worker: flush_if_dirty threw: " << e.what() << '\n');
            } catch (...) {
                LOG_CERR(ERROR, "[Messages] flush worker: flush_if_dirty threw unknown" << '\n');
            }
            return;
        }
        lk.unlock();
        try { flush_if_dirty(); }
        catch (const std::exception& e) {
            LOG_CERR(ERROR, "[Messages] flush worker: flush_if_dirty threw: " << e.what() << '\n');
        } catch (...) {
            LOG_CERR(ERROR, "[Messages] flush worker: flush_if_dirty threw unknown" << '\n');
        }
    }
}

void Messages::clear() {
    {
        std::lock_guard<std::mutex> lk(messages_mutex_);
        messages_ = json::array();
        executing_tools_.clear();
        mark_dirty_locked();  // §PERSIST-RACE: publish the empty chain
    }
    // §CRASH-6 (monkeybyte report, defect #6): persist the cleared
    // state. The old code only reset the in-memory chain: it never set
    // dirty_, so the auto-flush worker never wrote the empty state, and
    // the SQLite mirror (which load() consults FIRST) kept the old
    // chain forever — a corrupt chain survived "clear chat" until the
    // whole .daedalus/ folder was deleted. save() on an empty chain
    // removes messages.json, the .tmp/.crash mirrors, the SQLite mirror
    // row, and clears the crash payload.
    if (config_.auto_flush_enabled) {
        flush_cv_.notify_all();
    }
    save();
}

bool Messages::cleanup_dangling_tool_results() {
    std::lock_guard<std::mutex> lk(messages_mutex_);
    if (messages_.empty()) return false;

    std::unordered_set<std::string> removed_tool_ids;
    bool removed_any = false;

    // Remove trailing tool results. These occur when the previous turn was
    // interrupted after a tool executed but before the assistant could respond.
    while (!messages_.empty() && chain_safe_str(messages_.back(), "role") == "tool") {
        std::string tc_id = chain_safe_str(messages_.back(), "tool_call_id");
        if (!tc_id.empty()) {
            removed_tool_ids.insert(tc_id);
        }
        messages_.erase(messages_.end() - 1);
        removed_any = true;
    }

    // If the message that initiated those tool calls is now at the end,
    // remove it as well — an assistant message with tool_calls must be
    // followed by tool results, not a user message.
    if (!messages_.empty() && chain_safe_str(messages_.back(), "role") == "assistant"
        && messages_.back().contains("tool_calls")) {
        const auto& tool_calls = messages_.back()["tool_calls"];
        bool all_have_removed_results = true;
        if (tool_calls.is_array()) {
            for (const auto& tc : tool_calls) {
                std::string tc_id = chain_safe_str(tc, "id");
                if (!tc_id.empty() && removed_tool_ids.count(tc_id) == 0) {
                    all_have_removed_results = false;
                    break;
                }
            }
        }
        if (all_have_removed_results) {
            messages_.erase(messages_.end() - 1);
            removed_any = true;
        }
    }

    if (removed_any) {
        mark_dirty_locked();  // §PERSIST-RACE (lock held by this function)
        // PERSIST-DIRTY: wake the worker immediately - this cleanup runs
        // at the entry of a turn after an interrupted one, and its result
        // (an unanswered user message at the tail) was previously the
        // stale state that outlived the session on disk.
        if (config_.auto_flush_enabled) {
            flush_cv_.notify_all();
        }
    }
    return removed_any;
}

bool Messages::has_saved_messages() const {
    return std::filesystem::exists(messages_file_);
}

// ============================================================================
// Compression
// ============================================================================

bool Messages::should_compress(size_t current_tokens) const {
    float ratio = get_compression_ratio(current_tokens);
    return ratio >= config_.compress_threshold;
}

float Messages::get_compression_ratio(size_t current_tokens) const {
    if (config_.max_context_size == 0) return 0.0f;
    return static_cast<float>(current_tokens) / static_cast<float>(config_.max_context_size);
}

// §B13 refactor: extract the safe split-point computation to a shared helper.
// Both split_messages() and get_compression_indices() previously duplicated
// this logic and had already drifted slightly. Now they share one source.
size_t Messages::compute_safe_split() const {
    // Count non-system messages (index 0 is always system)
    size_t total_non_system = 0;
    for (size_t i = 1; i < messages_.size(); ++i) {
        total_non_system++;
    }
    if (total_non_system == 0) return 0;

    // Target compress_ratio of non-system messages
    size_t target = static_cast<size_t>(total_non_system * config_.compress_ratio);
    if (target == 0) return 0;

    // Collect tool_call ids from assistant messages in the [1, target] range
    std::unordered_set<std::string> first_ids;
    for (size_t i = 1; i <= target && i < messages_.size(); ++i) {
        const auto& m = messages_[i];
        if (m.value("role", "") == "assistant" && m.contains("tool_calls")) {
            for (const auto& tc : m["tool_calls"]) {
                std::string id = tc.value("id", "");
                if (!id.empty()) first_ids.insert(id);
            }
        }
    }

    // Extend target to include any tool-result that belongs to a first_ids call
    // (so the full chain is kept in the compressed region, not orphaned).
    // §fix (2026-07-18): off-by-one — `target` is a COUNT of non-system
    // messages (absolute index i is included when i <= target), but `i`
    // here is an ABSOLUTE index. `target = i + 1` pulled one extra
    // message into the compressed region.
    bool changed = true;
    while (changed) {
        changed = false;
        for (size_t i = 1; i < messages_.size(); ++i) {
            const auto& m = messages_[i];
            if (m.value("role", "") == "tool") {
                std::string id = m.value("tool_call_id", "");
                if (!id.empty() && first_ids.count(id) > 0) {
                    if (i > target) {
                        target = i;
                        changed = true;
                    }
                    break;  // one extension per tool-result is enough
                }
            }
        }
    }

    return std::min(target, total_non_system);
}

void Messages::split_messages(json& prefix, json& middle, json& suffix) const {
    // Split at (compress_ratio * 100)% / ((1 - compress_ratio) * 100)%
    // Default: 70% / 30% - compress the oldest 70%, keep the newest 30%
    //
    // CRITICAL: Ensure we don't split in the middle of a tool call pair.
    // A tool call pair is:
    //   [assistant, tool_calls: [{id: "X"}]]   <- tool call
    //   [tool, tool_call_id: "X", content]      <- tool result
    // If we split between these, the tool result becomes orphaned
    // (its tool_call_id is in the compressed section that no longer exists).
    //
    // Uses compute_safe_split() — the shared helper ensures both
    // split_messages and get_compression_indices always agree.

    prefix = json::array();
    if (!messages_.empty() && messages_[0].value("role", "") == "system") {
        prefix.push_back(messages_[0]);  // Keep system message in prefix
    }

    middle = json::array();
    suffix = json::array();

    size_t target = compute_safe_split();

    // Build middle (compressed) and suffix (kept) from the split point.
    // Non-system index 0 → messages_[1], non-system index 1 → messages_[2], ...
    for (size_t i = 1; i < messages_.size(); ++i) {
        size_t non_sys_idx = i - 1;
        if (non_sys_idx < target) {
            middle.push_back(messages_[i]);
        } else {
            suffix.push_back(messages_[i]);
        }
    }
}

void Messages::get_split_info(size_t& prefix_size, size_t& middle_size, size_t& suffix_size) const {
    json prefix, middle, suffix;
    split_messages(prefix, middle, suffix);
    prefix_size = prefix.size();
    middle_size = middle.size();
    suffix_size = suffix.size();
}

std::pair<size_t, size_t> Messages::get_compression_indices() const {
    // §B13 refactor: uses compute_safe_split() — same algorithm as split_messages.
    size_t target = compute_safe_split();
    return {1, 1 + target};  // exclusive range
}

// Phase 11: removed dead Messages::build_compression_prompt(prefix,
// middle, suffix) — superseded by Messages::build_compression_prompt(
// middle_plain_text). The 3-arg version was never called; its prompt
// template duplicated the work and the JSON serialization was an extra
// ~50% size hit. The 1-arg version (called from compress_messages)
// uses plain-text serialization which is faster.

std::string Messages::call_llm_for_summary(const std::string& prompt) {
    // ============================================================================
    // COMPRESSION TIMEOUT STRATEGY (DO NOT REMOVE THESE COMMENTS)
    // ============================================================================
    // We try the compression up to 3 times with INCREASING timeouts derived
    // from config_.compression_timeout (default 30s):
    //   Attempt 1: 1x base (default 30s)  - fast path, works for small contexts (10-100 msgs)
    //   Attempt 2: 2x base (default 60s)  - gives more time for medium contexts
    //   Attempt 3: 4x base (default 120s) - for very large contexts (800+ messages)
    //
    // We do NOT use exponential backoff (wait longer between retries) because
    // that would just delay the inevitable. Instead, we give each retry MORE
    // TIME on the API call itself (the timeout grows).
    //
    // WHY EACH RETRY GETS MORE TIME: A 30s timeout might fail simply because
    // the LLM is slow to respond to a large prompt (800+ messages can take
    // minutes to summarize). A 60s or 120s timeout gives the model the time
    // it needs to finish. We don't want to give up early.
    //
    // IF ALL 3 RETRIES FAIL: We return empty string. The caller (compress())
    // will then fall back to DELETING THE FIRST 70% OF MESSAGES as a
    // last-resort measure to prevent context overflow. We have NO OTHER
    // CHOICE - if the API is down or rate-limited, the only way to keep
    // the agent running is to shrink the context manually. Better to lose
    // 70% of context than to have the agent crash from overflow.
    // ============================================================================

    // Build request payload
    json msg_content = json::array();
    msg_content.push_back(json{{"role", "user"}, {"content", prompt}});

    json request_payload = json::object();
    request_payload["model"] = config_.model;
    request_payload["messages"] = msg_content;
    request_payload["stream"] = false;
    // §BUG-COMPRESS-TEMP (DO NOT REMOVE): config_.temperature MUST already
    // be the provider-plugin-clamped value — LLMClient clamps it via
    // plugin_->clamp_temperature() when populating this config (constructor
    // and set_config()). This compression call bypasses run_agent_cycle(),
    // so it never sees the clamp the main chat path applies. Models with a
    // fixed server-side temperature (Kimi k2.7-code / k3 / kimi-for-coding
    // = 1.0) HARD-REJECT any other value, which used to fail all 3 attempts
    // and silently delete 70% of the context via the fallback below.
    // If you touch this, keep tests/test_auto_compress.cpp's
    // "compression request carries provider-clamped temperature" green.
    request_payload["temperature"] = config_.temperature;
    request_payload["max_tokens"] = config_.max_compress_output;

    std::string request_body = request_payload.dump();

    // Timeout sequence: base, 2x base, 4x base (2x growth per retry).
    // The base comes from config_.compression_timeout (default 30s →
    // 30s/60s/120s). DO NOT CHANGE WITHOUT READING THE COMMENT BLOCK ABOVE
    const int base_timeout = std::max(1, config_.compression_timeout);
    const int timeouts[] = {base_timeout, base_timeout * 2, base_timeout * 4};
    const int num_attempts = sizeof(timeouts) / sizeof(timeouts[0]);

    using hive::net::HttpRequest;
    using hive::net::HttpSession;
    using hive::net::HttpError;

    for (int attempt = 0; attempt < num_attempts; ++attempt) {
        HttpRequest req;
        req.url = config_.api_url;
        req.method = "POST";
        req.body = request_body;
        req.headers.push_back("Content-Type: application/json");
        req.headers.push_back("Accept: application/json");
        if (!config_.api_key.empty()) {
            req.bearer_token = config_.api_key;
        }
        req.timeout_seconds = timeouts[attempt];  // 30s, 60s, 120s
        req.follow_redirects = true;

        LOG_CERR(INFO,  "[Messages] Compression attempt " << (attempt + 1) << "/"
                  << num_attempts << " (timeout=" << timeouts[attempt]
                  << "s, prompt_size=" << prompt.size() << " bytes)" << '\n');

        auto fut = HttpSession::get_instance().submit(req);
        auto result = fut.get();

        if (result && result->status_code >= 200 && result->status_code < 300) {
            // SUCCESS — extract summary text AND usage for token reporting
            LOG_CERR(INFO,  "[Messages] Compression attempt " << (attempt + 1)
                      << " succeeded" << '\n');
            // §Phase 9.10: extract usage from compression response so the main
            // agent's token bar is accurate. (MiniMax SSE is always usage=null.)
            if (config_.on_compression_tokens) {
                try {
                    json resp = json::parse(result->body);
                    if (resp.contains("usage") && !resp["usage"].is_null()) {
                        int pt = resp["usage"].value("prompt_tokens", 0);
                        int ct = resp["usage"].value("completion_tokens", 0);
                        int tt = resp["usage"].value("total_tokens", 0);
                        LOG_CERR(INFO,  "[Messages] Compression usage: prompt=" << pt
                                  << " completion=" << ct << " total=" << tt << '\n');
                        config_.on_compression_tokens(pt, ct, tt);
                    } else {
                        LOG_CERR(INFO,  "[Messages] Compression response has no usage data"
                                         " — skipping token report" << '\n');
                    }
                } catch (const std::exception& e) {
                    LOG_CERR(INFO,  "[Messages] Could not parse compression usage: "
                                  << e.what() << '\n');
                }
            }
            return extract_summary_text(result->body);
        }

        if (!result) {
            HttpError err = result.error();
            LOG_CERR(ERROR,  "[Messages] Compression attempt " << (attempt + 1)
                      << " failed: " << hive::net::to_string(err)
                      << " (url=" << req.url
                      << ", timeout=" << timeouts[attempt] << "s)" << '\n');
        } else {
            // HTTP error (4xx/5xx) - log it but retry
            LOG_CERR(ERROR,  "[Messages] Compression attempt " << (attempt + 1)
                      << " failed: HTTP " << result->status_code << '\n');
            if (!result->body.empty()) {
                std::string body_snippet = result->body.substr(0, 200);
                LOG_CERR(INFO,  "[Messages] Response body: " << body_snippet << '\n');
            }
            // Do NOT retry on 4xx (client errors) - they won't fix themselves
            if (result->status_code >= 400 && result->status_code < 500) {
                LOG_CERR(INFO,  "[Messages] HTTP 4xx is a client error, not retrying" << '\n');
                return {};
            }
            // 5xx errors might be transient - retry
        }
    }

    // ALL 3 ATTEMPTS FAILED. Return empty string.
    // The caller (compress()) will see this and fall back to deleting
    // the first 70% of messages to prevent context overflow.
    // We have NO OTHER CHOICE - if the API is down or rate-limited,
    // we MUST reduce context size to keep the agent running.
    LOG_CERR(ERROR,  "[Messages] All " << num_attempts << " compression attempts failed."
              << " Caller will fall back to deleting first 70% of messages."
              << '\n');
    return {};
}

std::string Messages::extract_summary_text(const std::string& response_body) {
    try {
        json response_json = json::parse(response_body);
        if (response_json.contains("choices") && !response_json["choices"].empty()) {
            std::string content = response_json["choices"][0]["message"]["content"].get<std::string>();
            
            // Remove <answer>...</answer> blocks if present
            size_t ans_start = content.find("<answer>");
            if (ans_start != std::string::npos) {
                size_t ans_end = content.find("</answer>");
                if (ans_end != std::string::npos) {
                    content = content.substr(ans_start + 8, ans_end - ans_start - 8);
                }
            }

            // Remove <thinking>...</thinking> blocks if present
            size_t thk_start = content.find("<thinking>");
            if (thk_start != std::string::npos) {
                size_t thk_end = content.find("</thinking>");
                if (thk_end != std::string::npos) {
                    content.erase(thk_start, thk_end - thk_start + 11);
                }
            }
            
            // Remove ```markdown blocks if present
            size_t md_start = content.find("```");
            if (md_start != std::string::npos) {
                size_t md_end = content.find("```", md_start + 3);
                if (md_end != std::string::npos) {
                    content = content.substr(md_start + 3, md_end - md_start - 3);
                }
            }
            
            // Trim whitespace
            while (!content.empty() && (content.front() == ' ' || content.front() == '\n')) {
                content.erase(content.begin());
            }
            while (!content.empty() && (content.back() == ' ' || content.back() == '\n')) {
                content.pop_back();
            }
            
            return content;
        }
    } catch (const std::exception& e) {
        LOG_CERR(INFO,  "[Messages] Error parsing response: " << e.what() << '\n');
    }
    
    return {};
}

CompressResult Messages::compress() {

    // §fix (2026-07-18): snapshot the compression range under
    // messages_mutex_ — the flush worker saves concurrently. The LLM
    // call below runs unlocked; messages_ is re-locked for the
    // mutation afterwards.
    size_t start_idx, end_idx, middle_count, before_count;
    json middle_copy;
    {
        std::lock_guard<std::mutex> lk(messages_mutex_);

        // Get the indices of messages to delete (given section)
        auto [s, e] = get_compression_indices();  // 70/30 split with safe point

        // If no messages, nothing to compress
        if (s >= e) {
            return {CompressOutcome::NothingToCompress, {}};
        }
        start_idx = s;
        end_idx = e;

        middle_count = end_idx - start_idx;
        before_count = messages_.size();

        // Make a COPY of messages to compress (do NOT delete originals yet)
        middle_copy = json::array();
        for (size_t i = start_idx; i < end_idx && i < messages_.size(); ++i) {
            middle_copy.push_back(messages_[i]);
        }
    }

    if (middle_copy.empty()) {
        return {CompressOutcome::NothingToCompress, {}};
    }

    // Write debug files if enabled
    if (config_.debug_output) {
        write_debug_file("daedalus_compress_middle.txt", middle_copy.dump(2));
    }

    // Use plain text format - removes ~50% overhead vs JSON
    std::string middle_plain = messages_to_plain_text(middle_copy);

    // Build compression prompt
    std::string prompt = Messages::build_compression_prompt(middle_plain);

    // CRITICAL FIX: Call LLM for summary BEFORE deleting any messages.
    // If the API call fails, the user keeps all their messages.
    // Old behavior was to delete first, which destroyed context on any failure.
    std::string summary = call_llm_for_summary(prompt);

    if (summary.empty()) {
        // ============================================================================
        // COMPRESSION FAILED - LAST RESORT FALLBACK (DO NOT REMOVE)
        // ============================================================================
        // The compression API call failed (3 retries with timeouts 30s, 60s, 120s
        // all timed out or errored). We have NO OTHER CHOICE but to delete the
        // first 70% of messages to prevent context overflow. If we don't, the
        // next API call will exceed the model's context window and the agent
        // will crash. Better to lose 70% of context than to crash entirely.
        //
        // The agent will lose the early messages but keep:
        //   - The system message (first)
        //   - The last 30% of messages (recent context)
        // This gives the agent enough context to continue working, even if
        // it loses the history of what it was doing earlier.
        // ============================================================================

        std::lock_guard<std::mutex> lk(messages_mutex_);
        size_t total = messages_.size();
        if (total <= 5) {
            // Too few messages to safely delete 70% - just give up
            LOG_CERR(ERROR,  "[CONTEXT] Compression failed and only " << total
                      << " messages remain. Cannot safely reduce. Giving up."
                      << '\n');
            return {CompressOutcome::Failed, {}};
        }

        // Calculate the safe split point: keep system + last 30%
        size_t keep_count = std::max(size_t(2), total * 30 / 100);  // At least system + 1 other
        size_t delete_count = total - keep_count;

        // Find the start index for deletion (skip system message at index 0)
        size_t delete_start = (total > 0 && messages_[0].value("role", "") == "system") ? 1 : 0;
        // But make sure we don't delete more than delete_count
        if (delete_start >= total) delete_start = 0;
        size_t actual_delete_count = std::min(delete_count, total - delete_start);

        if (actual_delete_count == 0) {
            LOG_CERR(WARNING,  "[CONTEXT] Cannot safely reduce context (only system msg?). Giving up."
                      << '\n');
            return {CompressOutcome::Failed, {}};
        }

        LOG_CERR(ERROR,  "[CONTEXT] FALLBACK: All compression attempts failed."
                  << '\n');
        LOG_CERR(INFO,  "[CONTEXT] FALLBACK: Deleting first " << actual_delete_count
                  << " of " << total << " messages to prevent context overflow."
                  << '\n');
        LOG_CERR(INFO,  "[CONTEXT] FALLBACK: Keeping system message + last "
                  << (total - actual_delete_count) << " messages."
                  << '\n');

        messages_.erase(messages_.begin() + delete_start,
                        messages_.begin() + delete_start + actual_delete_count);
        // §PERSIST-RACE: the fallback rewrite was never marked dirty,
        // so the reduced chain could be lost if the process died before
        // the next add_*(). Publish it now (lock held above).
        mark_dirty_locked();
        // §COMPRESS-REPORT: the context WAS reduced — report it as
        // FallbackDeleted so callers fire on_compaction and do NOT
        // schedule a retry. (Previously this returned the same nullopt
        // as a genuine failure, so hosts never saw the state change.)
        return {CompressOutcome::FallbackDeleted, {}};
    }

    // SUCCESS: Now safe to delete the middle messages and rebuild
    // §fix (2026-07-18): the mutation below runs under messages_mutex_
    // (the flush worker saves concurrently).
    json new_messages;
    {
        std::lock_guard<std::mutex> lk(messages_mutex_);

        messages_.erase(messages_.begin() + start_idx, messages_.begin() + end_idx);
        // §Phase 5.1: route through Logger instead of std::cout.
        LOG_CERR(INFO, "[CONTEXT] Deleted " << middle_count << " middle messages (indices "
                  << start_idx << "-" << end_idx << ")");

        // SUCCESS: Rebuild messages in correct order
        // Structure: system + user(summary) + suffix (last 50%)
        new_messages = json::array();

        // 1. Add system message (must be first)
        if (!messages_.empty() && messages_[0].value("role", "") == "system") {
            new_messages.push_back(messages_[0]);
        }

        // 2. §COMPRESS-USER-MSG (2026-08-01): strict chat templates
        //    (qwen3's jinja on LM Studio: "No user query found in
        //    messages") require the FIRST message after system to be a
        //    user message — verified empirically: assistant/tool-first
        //    histories are rejected even when a user message exists
        //    later. The summary rides inside this synthetic user
        //    message: no assistant/tool pair is emitted (the
        //    model-facing compress_context tool was removed), so the
        //    rebuilt chain stays valid for every provider.
        new_messages.push_back({
            {"role", "user"},
            {"content", "[Context compressed — the earlier conversation "
                        "was summarized. Summary of the removed messages:\n"
                        + summary +
                        "\nContinue the current task.]"}
        });

        // 3. Add suffix messages (last remaining messages already in messages_ after deletion)
        //    These are the messages AFTER the given messages we just deleted
        //    (messages_ currently has: system + suffix)
        for (size_t i = 1; i < messages_.size(); ++i) {  // Start at 1 to skip system
            new_messages.push_back(messages_[i]);
        }

        // Clear ephemeral executing markers after compression
        executing_tools_.clear();
    }

    size_t after_count = new_messages.size();

    // §Phase 5.1: route through Logger instead of std::cout.
    LOG_CERR(INFO, "\n[CONTEXT] Compressed " << before_count << " messages to " << after_count << " messages.");
    LOG_CERR(INFO, "[CONTEXT] Structure: system + user(summary) + suffix(last 50%)");
    LOG_CERR(INFO, "[CONTEXT] Messages summarized: " << middle_count << " messages -> 1 summary");

    std::string result = new_messages.dump();

    return {CompressOutcome::Compressed, result};
}

void Messages::replace_messages(const std::string& compressed_json) {
    try {
        json parsed = json::parse(compressed_json);
        const size_t count = parsed.size();
        {
            std::lock_guard<std::mutex> lk(messages_mutex_);
            messages_ = std::move(parsed);
            // §PERSIST-RACE: the compressed chain rewrite was never
            // marked dirty - publish it so the write-through worker
            // persists the compressed state promptly.
            mark_dirty_locked();
        }
        // §Phase 5.1: route through Logger instead of std::cout.
        LOG_CERR(INFO, "[CONTEXT] Replaced " << count << " messages with compressed version");
    } catch (const std::exception& e) {
        LOG_CERR(ERROR,  "[Messages] Failed to replace messages: " << e.what() << '\n');
    }
}

// ============================================================================
// Tool Call Lifecycle Management (Single Source of Truth)
// ============================================================================

bool Messages::mark_tool_executing(const std::string& id) {
    // First check if tool exists (has assistant with tool_calls[id])
    int assistant_idx = find_assistant_with_tool(id);
    if (assistant_idx < 0) {
        LOG_CERR(INFO,  "[Messages] mark_tool_executing: " << id << " not found in any assistant" << '\n');
        return false;
    }
    
    // Check state - can only go from PENDING to EXECUTING
    ToolState state = get_tool_state(id);
    if (state != ToolState::PENDING) {
        LOG_CERR(INFO,  "[Messages] mark_tool_executing: " << id << " not in PENDING state (current: " << static_cast<int>(state) << ")" << '\n');
        return false;
    }
    
    // Set ephemeral executing marker
    executing_tools_.insert(id);
    LOG_DEBUG("Tool " + id + " marked EXECUTING");
    return true;
}

void Messages::clear_executing(const std::string& id) {
    executing_tools_.erase(id);
}

bool Messages::is_executing(const std::string& id) const {
    return executing_tools_.count(id) > 0;
}

Messages::ToolState Messages::get_tool_state(const std::string& id) const {
    // 1. Check if executing (ephemeral runtime state)
    if (executing_tools_.count(id) > 0) return ToolState::EXECUTING;
    
    // 2. Check if tool result exists
    if (has_tool_result(id)) {
        return is_error_result(id) ? ToolState::FAILED : ToolState::COMPLETED;
    }
    
    // 3. Check if tool call exists in any assistant message
    if (find_assistant_with_tool(id) >= 0) return ToolState::PENDING;
    
    return ToolState::INVALID;
}

bool Messages::all_tools_resolved() const {
    // Find all tool call IDs from assistant messages
    std::vector<std::string> pending_ids = get_pending_tool_ids();
    return pending_ids.empty();
}

bool Messages::has_pending_tools() const {
    return !get_pending_tool_ids().empty();
}

bool Messages::has_executing_tools() const {
    return !executing_tools_.empty();
}

std::vector<std::string> Messages::get_pending_tool_ids() const {
    std::vector<std::string> pending;
    
    for (const auto& msg : messages_) {
        if (msg.value("role", "") == "assistant" && msg.contains("tool_calls")) {
            for (const auto& tc : msg["tool_calls"]) {
                // SANITIZE-NOTHROW: tc may be a non-object (e.g. a bare
                // string) in a corrupt chain - value() would throw.
                std::string tc_id = chain_safe_str(tc, "id");
                if (!tc_id.empty()) {
                    // Check if there's a result for this tool
                    if (!has_tool_result(tc_id)) {
                        pending.push_back(tc_id);
                    }
                }
            }
        }
    }
    
    return pending;
}

namespace {

// SANITIZE-NOTHROW (2026-08-31, MonkeyByte coredump 2026-08-30):
// entry.value("role", ...) throws type_error when a chain entry is not
// an object, or when role/tool_call_id/id is present but null or
// non-string. That exception escaped save() onto the background flush
// worker thread -> std::terminate -> SIGABRT (intermittent: fired only
// when a malformed/partially-written chain entry met a flush tick).
// Sanitize runs on the persistence path, so it must never throw --
// extract strings defensively and drop unusable entries instead.
std::string chain_safe_str(const json& j, const char* key) {
    if (!j.is_object()) return "";
    const auto it = j.find(key);
    return (it != j.end() && it->is_string()) ? it->get<std::string>()
                                              : std::string();
}

} // namespace

json Messages::sanitize_assistant_message(const json& msg,
                                            const std::unordered_set<std::string>& tool_result_ids) const {
    if (!msg.is_object() || !msg.contains("tool_calls")) return msg;

    json sanitized = msg;
    json valid_calls = json::array();

    if (msg["tool_calls"].is_array()) {
        for (const auto& tc : msg["tool_calls"]) {
            // SANITIZE-NOTHROW: a non-object tool_call entry (null, string,
            // ...) would make .value()/.contains() below throw type_error.
            if (!tc.is_object()) {
                LOG_CERR(INFO,  "[Messages] Removing non-object tool_call entry" << '\n');
                continue;
            }
            std::string tc_id = chain_safe_str(tc, "id");

            bool valid_args = true;
            if (tc.contains("function") && tc["function"].is_object()
                && tc["function"].contains("arguments")) {
                const auto& args_val = tc["function"]["arguments"];
                if (args_val.is_string()) {
                    // Use json::accept() for efficient validation without creating parse tree
                    valid_args = json::accept(args_val.get<std::string>());
                }
            }
            
            // Remove ORPHAN tool_calls (those with no matching tool result).
            // These cause API 400 errors and infinite retry loops.
            // §fix (2026-07-30): the set passed in now contains only the
            // results ADJACENT to this assistant (sanitize_message_chain is
            // position-aware), so a call whose result exists only elsewhere
            // in history is pruned here as well. An empty set prunes all
            // tool_calls — a pending call without results is not sendable.
            bool has_result = !tool_result_ids.empty() &&
                              tool_result_ids.count(tc_id) > 0;
            if (!has_result) {
                LOG_CERR(INFO,  "[Messages] Removing orphan tool_call (no result): " << tc_id << '\n');
                continue;
            }

            if (valid_args) {
                valid_calls.push_back(tc);
            } else {
                LOG_CERR(INFO,  "[Messages] Removing invalid args: " << tc_id << '\n');
            }
        }
    }
    
    if (valid_calls.empty()) {
        sanitized.erase("tool_calls");
    } else {
        sanitized["tool_calls"] = valid_calls;
    }
    
    return sanitized;
}

json Messages::sanitize_message_chain(const json& msgs) const {
    if (!msgs.is_array()) return msgs;

    json result = json::array();

    // §fix (2026-07-30): position-aware validation.
    //
    // The provider requires a `role:"tool"` message to DIRECTLY follow the
    // assistant message carrying its `tool_calls` (or a sibling tool result
    // in the same contiguous block). The previous implementation only checked
    // that the `tool_call_id` existed SOMEWHERE in history, so a misplaced or
    // duplicated tool result survived sanitization and the API rejected the
    // whole request on every retry (infinite HTTP 400 loop).
    //
    // New model: an assistant with tool_calls OPENS a block; the contiguous
    // run of tool messages immediately after it may answer those calls; any
    // non-tool message CLOSES the block. Tool results that arrive outside
    // their block (misplaced), or twice (duplicate), are dropped. Assistant
    // tool_calls without an adjacent result are pruned (they are equally
    // invalid for the API).
    std::unordered_set<std::string> open_block;        // ids answerable now
    std::unordered_set<std::string> answered_in_block; // ids already answered

    for (size_t i = 0; i < msgs.size(); i++) {
        const auto& msg = msgs[i];
        if (!msg.is_object()) {
            LOG_CERR(INFO,  "[Messages] Dropped non-object chain entry at index " << i << '\n');
            continue;
        }
        std::string role = chain_safe_str(msg, "role");

        if (role == "tool") {
            std::string tc_id = chain_safe_str(msg, "tool_call_id");
            if (tc_id.empty()) {
                LOG_CERR(INFO,  "[Messages] Removed malformed tool result (empty tool_call_id)" << '\n');
                continue;
            }
            if (open_block.count(tc_id) == 0) {
                // Orphaned (no matching tool call) OR misplaced (its parent
                // assistant is not the immediately preceding message).
                LOG_CERR(INFO,  "[Messages] Removed orphaned/misplaced tool result (no open block for id: " << tc_id << ")" << '\n');
                continue;
            }
            if (answered_in_block.count(tc_id) > 0) {
                LOG_CERR(INFO,  "[Messages] Removed duplicate tool result (id: " << tc_id << ")" << '\n');
                continue;
            }
            answered_in_block.insert(tc_id);
            result.push_back(msg);
            continue;
        }

        // Any non-tool message closes the current block.
        open_block.clear();
        answered_in_block.clear();

        if (role == "assistant" && msg.contains("tool_calls")
            && msg["tool_calls"].is_array()) {
            // Collect the ids answered by the contiguous run of tool
            // messages immediately following this assistant. Results
            // elsewhere in history do NOT count (position-aware).
            std::unordered_set<std::string> adjacent_result_ids;
            for (size_t j = i + 1; j < msgs.size(); j++) {
                if (chain_safe_str(msgs[j], "role") != "tool") break;
                std::string id = chain_safe_str(msgs[j], "tool_call_id");
                if (!id.empty()) adjacent_result_ids.insert(id);
            }
            // Prune tool_calls that have no adjacent result or invalid args.
            json sanitized = sanitize_assistant_message(msg, adjacent_result_ids);
            if (sanitized.contains("content") || sanitized.contains("tool_calls")) {
                result.push_back(sanitized);
                if (sanitized.contains("tool_calls")) {
                    for (const auto& tc : sanitized["tool_calls"]) {
                        std::string id = chain_safe_str(tc, "id");
                        if (!id.empty()) open_block.insert(id);
                    }
                }
            }
        } else {
            result.push_back(msg);
        }
    }

    return result;
}

// §fix (2026-07-30): guarded synthetic tool-result injection.
// The previous "CRITICAL FIX" recovery in LLMClient appended a tool result
// for the last tool_call found ANYWHERE in history — including calls that
// were already answered mid-conversation. The appended tool message then
// followed a user/assistant message instead of its parent assistant, which
// the provider rejects ("messages with role 'tool' must be a response to a
// preceeding message with 'tool_calls'"). Worse, the old sanitizer only
// checked that the id existed somewhere, so the bogus message survived
// every retry and the recovery re-fired — an infinite HTTP 400 loop that
// grew the chain by two messages per cycle.
bool Messages::append_tool_results_if_pending(json& messages,
                                              const std::string& content) {
    if (!messages.is_array() || messages.empty()) return false;

    // Locate the last assistant message that carries tool_calls.
    int k = -1;
    for (int i = static_cast<int>(messages.size()) - 1; i >= 0; i--) {
        if (!messages[i].is_object()) continue;
        if (messages[i].value("role", "") != "assistant") continue;
        if (messages[i].contains("tool_calls") &&
            messages[i]["tool_calls"].is_array() &&
            !messages[i]["tool_calls"].empty()) {
            k = i;
            break;
        }
    }
    if (k < 0) return false;

    // Collect the block's tool_call ids (in order).
    std::vector<std::string> block_ids;
    for (const auto& tc : messages[k]["tool_calls"]) {
        std::string id = tc.value("id", "");
        if (!id.empty()) block_ids.push_back(id);
    }
    if (block_ids.empty()) return false;

    // Every message after k must be a tool result of this block —
    // otherwise the call is already closed mid-history and injecting a
    // tool result at the tail would create an invalid sequence.
    std::unordered_set<std::string> answered;
    for (size_t i = static_cast<size_t>(k) + 1; i < messages.size(); i++) {
        if (!messages[i].is_object()) return false;
        if (messages[i].value("role", "") != "tool") return false;
        std::string id = messages[i].value("tool_call_id", "");
        if (id.empty() ||
            std::find(block_ids.begin(), block_ids.end(), id) == block_ids.end()) {
            return false;
        }
        answered.insert(id);
    }

    // Append results for the ids that are still missing, in block order.
    bool appended = false;
    for (const auto& id : block_ids) {
        if (answered.count(id) > 0) continue;
        messages.push_back({
            {"role", "tool"},
            {"tool_call_id", id},
            {"content", content}
        });
        appended = true;
    }
    return appended;
}

void Messages::clear_executing_markers() {
    executing_tools_.clear();
}

// ============================================================================
// Private Helper Methods
// ============================================================================

int Messages::find_assistant_with_tool(const std::string& id) const {
    for (size_t i = 0; i < messages_.size(); i++) {
        const auto& msg = messages_[i];
        if (msg.value("role", "") == "assistant" && msg.contains("tool_calls")) {
            for (const auto& tc : msg["tool_calls"]) {
                if (tc.value("id", "") == id) {
                    return static_cast<int>(i);
                }
            }
        }
    }
    return -1;
}

bool Messages::has_tool_result(const std::string& id) const {
    for (const auto& msg : messages_) {
        if (msg.value("role", "") == "tool" && 
            msg.value("tool_call_id", "") == id) {
            return true;
        }
    }
    return false;
}

bool Messages::is_error_result(const std::string& id) const {
    for (const auto& msg : messages_) {
        if (msg.value("role", "") == "tool" && 
            msg.value("tool_call_id", "") == id) {
            // First check the structured status field (preferred)
            std::string status = msg.value("status", "");
            if (status == "error") {
                return true;
            }
            if (status == "success") {
                return false;
            }
            // Fallback: check content for error indicators (backward compatibility)
            // Only check if status is not explicitly set
            std::string content = msg.value("content", "");
            return content.find("Error:") != std::string::npos ||
                   content.find("error") != std::string::npos;
        }
    }
    return false;
}

// ============================================================================
// Helpers
// ============================================================================

size_t Messages::estimate_tokens() const {
    std::lock_guard<std::mutex> lk(messages_mutex_);
    return estimate_tokens_from_json(messages_);
}

size_t Messages::estimate_tokens_from_json(const json& j) const {
    return j.dump().size() / 4;  // Rough estimate: 1 token ≈ 4 bytes
}

void Messages::inject_context_stats(size_t current_tokens, size_t iteration, size_t max_iterations) {
    std::lock_guard<std::mutex> lk(messages_mutex_);
    if (messages_.empty()) return;

    json& system = messages_[0];
    if (system.value("role", "") != "system") return;

    std::string content = system.value("content", "");

    // Build the stats block. §B8 fix: append a sentinel line so we
    // can find and erase exactly what we wrote instead of walking
    // forward to the next same-level heading (the old behaviour
    // dropped the trailing tail of the prompt when the stats block
    // was the LAST section).
    std::ostringstream stats;
    stats << "\n\n## Context Status\n";
    stats << "- **Token Usage**: " << current_tokens << "/" << config_.max_context_size
          << " (" << static_cast<int>(get_compression_ratio(current_tokens) * 100) << "%)\n";
    stats << "- **Iterations**: " << iteration << "/" << max_iterations << "\n";
    stats << "- **Messages**: " << messages_.size() << "\n";
    constexpr const char* kEndSentinel = "<!-- /daedalus-context-status -->\n";
    stats << kEndSentinel;

    // Find or append Context Status section.
    size_t ctx_pos = content.find("## Context Status");
    if (ctx_pos != std::string::npos) {
        // Locate the previous block's end (sentinel) and erase up to
        // and including it. If no sentinel (legacy pre-fix write),
        // walk forward to the next same-level heading, or to the next
        // blank line, but never to end-of-string.
        size_t sentinel_pos = content.find(kEndSentinel, ctx_pos);
        size_t end_pos;
        if (sentinel_pos != std::string::npos) {
            end_pos = sentinel_pos + std::strlen(kEndSentinel);
            content = content.substr(0, ctx_pos)
                    + stats.str()
                    + content.substr(end_pos);
        } else {
            size_t next_h = content.find("\n## ", ctx_pos + 1);
            if (next_h != std::string::npos) {
                end_pos = next_h;
                content = content.substr(0, ctx_pos)
                        + stats.str()
                        + content.substr(end_pos);
            } else {
                // No sentinel and no same-level heading: erase to the
                // next blank line (the stats block ended with one
                // before fix); if no blank line, append the new block
                // unchanged.
                size_t blank = content.find("\n\n", ctx_pos);
                if (blank != std::string::npos) {
                    content = content.substr(0, ctx_pos)
                            + stats.str()
                            + content.substr(blank);
                } else {
                    content = content.substr(0, ctx_pos) + stats.str();
                }
            }
        }
    } else {
        content += stats.str();
    }

    system["content"] = content;
}

void Messages::write_debug_file(const std::string& filename, const std::string& content) {
    std::ofstream debug_file("/tmp/" + filename);
    if (debug_file.is_open()) {
        debug_file << content << '\n';
        debug_file.close();
    }
}
json Messages::truncate_if_needed(size_t max_size) {
    // §fix (2026-07-18): this function rewrites messages_; the flush
    // worker saves concurrently, so hold messages_mutex_ throughout.
    std::lock_guard<std::mutex> lk(messages_mutex_);
    // §B5 fix: rewrite of the backward tool-chain walk.
    //
    // Pre-fix bug: the inner `while (i >= 1)` loop read `prev_role`
    // from `messages_[i]` BEFORE pushing, then checked it AFTER
    // pushing - leaving the truncated chain with an orphan tool
    // result on multi-step chains.
    //
    // New design: a helper `find_parent_assistant_for_tool_result`
    // walks backward looking for the assistant whose `tool_calls`
    // contains a matching `tool_call_id`. The outer loop accepts
    // messages from the end; when it hits a tool result it asks the
    // helper where the chain starts, includes everything in one go,
    // and jumps the index past it.

    std::function<void(json&)> sanitize_json_for_truncation;
    sanitize_json_for_truncation = [&](json& j) {
        if (j.is_object()) {
            for (auto& [key, value] : j.items()) {
                if (value.is_string()) {
                    value = utf8_sanitize_replace_invalid(value.get<std::string>());
                } else if (value.is_object() || value.is_array()) {
                    sanitize_json_for_truncation(value);
                }
            }
        } else if (j.is_array()) {
            for (auto& item : j) {
                if (item.is_string()) {
                    item = utf8_sanitize_replace_invalid(item.get<std::string>());
                } else if (item.is_object() || item.is_array()) {
                    sanitize_json_for_truncation(item);
                }
            }
        }
    };
    sanitize_json_for_truncation(messages_);

    std::string serialized = messages_.dump();

    if (serialized.size() <= max_size) {
        return json::object();
    }

    LOG_CERR(WARNING, "[WARNING] Request payload ("
              << serialized.size() << " bytes) exceeds max_request_size ("
              << max_size << " bytes). Truncating oldest messages..." << '\n');

    if (messages_.size() <= 1) {
        LOG_ERROR("Cannot truncate - only system message present");
        return json::object({{"warning", "Request too large even for single message"}});
    }

    json system_msg = messages_[0];
    json truncated_messages = json::array();
    truncated_messages.push_back(system_msg);

    size_t current_size = system_msg.dump().size();
    size_t target_size = max_size * 0.8;

    auto find_parent_assistant_for_tool_result =
        [&](const std::string& tool_call_id) -> size_t {
        if (tool_call_id.empty()) return std::string::npos;
        for (size_t j = messages_.size(); j-- > 0; ) {
            const auto& m = messages_[j];
            // SANITIZE-NOTHROW: chain entries may be non-objects in a
            // corrupt chain; value() would throw type_error.306.
            if (chain_safe_str(m, "role") != "assistant") continue;
            if (!m.contains("tool_calls") || !m["tool_calls"].is_array()) continue;
            for (const auto& tc : m["tool_calls"]) {
                if (chain_safe_str(tc, "id") == tool_call_id) return j;
            }
        }
        return std::string::npos;
    };

    for (size_t i = messages_.size(); i-- > 1; ) {
        const std::string role = chain_safe_str(messages_[i], "role");

        if (role == "tool") {
            const std::string tool_call_id =
                chain_safe_str(messages_[i], "tool_call_id");
            const size_t parent =
                find_parent_assistant_for_tool_result(tool_call_id);

            if (parent == std::string::npos || parent < 1 || parent >= i) {
                // §fix (2026-07-30): orphan tool results are unsendable —
                // drop them here instead of letting the generic path below
                // push them into the truncated chain.
                LOG_CERR(WARNING, "[WARNING] Truncating orphan tool "
                          "result (tool_call_id='" << tool_call_id
                          << "')" << '\n');
                continue;
            } else {
                // §fix (2026-07-30): all-or-nothing block retention.
                // Pre-fix: the tool results in [parent+1, i] were pushed
                // first and the parent assistant was left to the generic
                // path — if it did not fit, the truncated chain kept tool
                // results without their parent assistant (which the API
                // rejects), and the stopped_early fall-through also pushed
                // messages_[i] a second time (duplicate). Now the whole
                // block [parent, i] is kept only when it fits complete;
                // otherwise it is dropped as a unit.
                size_t block_size = 0;
                for (size_t j = parent; j <= i; j++) {
                    block_size += messages_[j].dump().size();
                }
                if (current_size + block_size > target_size) {
                    LOG_CERR(WARNING, "[WARNING] Cannot keep complete "
                              "tool call chain - dropping it as a unit." << '\n');
                } else {
                    for (size_t j = i; j > parent; --j) {
                        truncated_messages.push_back(messages_[j]);
                    }
                    truncated_messages.push_back(messages_[parent]);
                    current_size += block_size;
                }
                // Skip the whole block; the for-update i-- moves past it.
                i = parent;
                continue;
            }
        }

        std::string msg_str = messages_[i].dump();
        if (current_size + msg_str.size() > target_size) {
            break;
        }
        truncated_messages.push_back(messages_[i]);
        current_size += msg_str.size();
    }

    json result = json::array();
    result.push_back(system_msg);
    for (size_t k = truncated_messages.size(); k-- > 1; ) {
        result.push_back(truncated_messages[k]);
    }

    std::string new_size_str = result.dump();
    LOG_CERR(INFO, "[INFO] Truncated from " << serialized.size() << " to "
              << new_size_str.size() << " bytes (" << result.size()
              << " messages)" << '\n');

    messages_ = result;
    // §PERSIST-RACE: the truncation rewrite was never marked dirty,
    // so the reduced chain could be lost (and the stale larger chain
    // reloaded) if the process died before the next add_*(). Publish
    // now - the lock is held for the whole function.
    mark_dirty_locked();
    return json::object({{"truncated", true},
                        {"original_size", serialized.size()},
                        {"new_size", new_size_str.size()}});
}
