#include "tools/BashCommandExecutor.h"
#include "core/Utf8Utils.h"  // §BUG-UTF8-TRUNC: utf8_truncate_safe
#include "tools/SanitizedOutputStream.h"
#include "core/Subprocess.h"
#include "core/ThreadPool.h"

#include <thread>
#include <atomic>
#include <sstream>
#include <algorithm>
#include <cstdlib>
#include <limits>
#include <regex>
#include <future>
#include <chrono>
#include <functional>

#include "core/Logger.h"

// §Phase 5.1: helper macro that converts LOG_CERR(INFO,  ...); style
// statements into LOG_INFO/LOG_WARNING/LOG_ERROR calls via the
// existing Logger. Same formatted text routed through the Logger
// instead of writing to stderr directly.
#define LOG_CERR(level, expr) do {                          \
        std::ostringstream _daedalus_msg;                   \
        _daedalus_msg << expr;                               \
        LOG_##level(_daedalus_msg.str());                    \
    } while (0)
// Spinner helper class for consistent animation
// Uses the shared ThreadPool (LOW priority) instead of creating a fresh
// std::thread per command. The thread-pool worker is reused.
class CommandSpinner {
public:
    explicit CommandSpinner(const std::string& cmd)
        : command_done(false), command(cmd) {
        using hive::concurrent::ThreadPool;
        using hive::concurrent::TaskPriority;
        // Submit to the shared pool - worker threads are reused
        spinner_future = ThreadPool::get_instance().submit(
            TaskPriority::LOW,
            [this]() { run(); }
        );
    }

    ~CommandSpinner() {
        stop();
    }

    void stop() {
        command_done = true;
        // Wait for the spinner task to complete
        if (spinner_future.valid()) {
            spinner_future.wait();
        }
    }

private:
    void run() {
        // §Phase 5.1: removed the spinner animation that wrote to
        // std::cout ("Executing: ...", "|/-\\", etc.). The library
        // is no longer responsible for terminal animation —
        // hosts that want a progress indicator should observe
        // the agent's sink events and render their own UI.
        // The thread still exists so the destructor/stop() call
        // sites remain valid; it just does nothing visible now.
        while (!command_done.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
    }

    std::atomic<bool> command_done;
    std::string command;
    std::future<void> spinner_future;
};

// ========== Public Interface ==========

BashCommandExecutor::Result BashCommandExecutor::execute(const std::string& command,
                                                          const std::string& base_dir,
                                                          int timeout_seconds) {
    return execute(command, base_dir, timeout_seconds, nullptr);
}

BashCommandExecutor::Result BashCommandExecutor::execute(
        const std::string& command,
        const std::string& base_dir,
        int timeout_seconds,
        std::function<void(std::string_view)> output_chunk_cb) {
    // Sanitize pkill command by removing -f flag
    // pkill -f matches ANY part of command line, not just process name
    std::string sanitized_cmd = sanitize_pkill_command(command);
    
    // Escape the base_dir for shell
    std::string escaped_dir = escape_shell_arg(base_dir);
    
    // Build the full command with cd to base_dir.
    // §FIX-heredoc-redirect: wrap the command in a brace group and
    // redirect the group instead of appending ` 2>&1` after the command.
    // Appending corrupted heredoc commands — the trailing `2>&1` landed
    // on the terminator line (`EOF` became `EOF 2>&1`). A trailing
    // newline before `}` terminates the command list (a `;` there would
    // be a syntax error for multi-line input).
    std::string full_command = "cd " + escaped_dir + " && { " + sanitized_cmd + "\n} 2>&1";
    
    return execute_internal(full_command, sanitized_cmd, timeout_seconds,
                            std::move(output_chunk_cb));
}

// ========== Private Implementation ==========

BashCommandExecutor::Result BashCommandExecutor::execute_internal(
        const std::string& full_command,
        const std::string& display_command,
        int timeout_seconds) {
    return execute_internal(full_command, display_command, timeout_seconds, nullptr);
}

BashCommandExecutor::Result BashCommandExecutor::execute_internal(
        const std::string& full_command,
        const std::string& display_command,
        int timeout_seconds,
        std::function<void(std::string_view)> output_chunk_cb) {
    
    CommandSpinner spinner(display_command);

    // Capture whether a chunk callback was supplied before moving it into
    // the sanitising stream wrapper.
    const bool has_chunk_cb = static_cast<bool>(output_chunk_cb);
    SanitizedOutputStream chunk_buffer(std::move(output_chunk_cb));

    // Use Subprocess for crash isolation with specified timeout. If a chunk
    // callback is provided, forward each raw output chunk through the buffer
    // so the host can display sanitised progress in real time.
    SubprocessResult result;
    if (has_chunk_cb) {
        result = Subprocess::execute(full_command, "", timeout_seconds * 1000,
            [&chunk_buffer](std::string_view chunk) {
                chunk_buffer.append(chunk);
            });
    } else {
        result = Subprocess::execute(full_command, "", timeout_seconds * 1000);
    }
    chunk_buffer.flush();
    
    spinner.stop();
    
    if (result.timed_out) {
        return Result(false, 124, "", "Command timed out after " + std::to_string(timeout_seconds) + " seconds");
    }
    
    return Result(result.exit_code == 0, result.exit_code, sanitize_output(result.stdout));
}

std::string BashCommandExecutor::build_spinner_context(const std::string& command) {
    // Extract first line for display
    std::istringstream iss(command);
    std::string first_word;
    iss >> first_word;
    return "Running: " + first_word;
}

// ========== Utilities ==========

std::string BashCommandExecutor::escape_shell_arg(const std::string& input) {
    std::string result;
    result.reserve(input.size() + 16);
    result.push_back('\'');
    for (char c : input) {
        if (c == '\'') {
            result.push_back('\'');
            result.push_back('\\');
            result.push_back('\'');
            result.push_back('\'');
        } else {
            result.push_back(c);
        }
    }
    result.push_back('\'');
    return result;
}

std::string BashCommandExecutor::sanitize_tty_sequences(const std::string& output) {
    // One-shot wrapper over the incremental sanitiser. Keeping this as
    // append()+flush() over SanitizedOutputStream guarantees one-shot and
    // streamed sanitisation can never diverge.
    return SanitizedOutputStream::sanitize(output);
}

std::string BashCommandExecutor::sanitize_output(const std::string& output) {
    if (output.empty()) {
        return output;
    }
    
    // Check first 512 bytes for binary content
    const size_t check_size = std::min(output.size(), static_cast<size_t>(512));
    size_t non_printable = 0;
    for (size_t i = 0; i < check_size; i++) {
        unsigned char c = static_cast<unsigned char>(output[i]);
        // Count non-printable characters (excluding common whitespace)
        if (c < 32 && c != '\t' && c != '\n' && c != '\r') {
            non_printable++;
        }
    }

    // If more than 10% non-printable, it's likely binary
    if (check_size > 0 && non_printable > check_size / 10) {
        return "[Binary output truncated - " + std::to_string(output.size()) + " bytes]";
    }

    // Strip TTY control sequences (cursor movement, screen erase, alternate
    // buffer, OSC, etc.) while preserving UTF-8 text and ANSI SGR colour/style
    // codes. This prevents TUI applications like htop/btop from corrupting the
    // host terminal when their captured output is forwarded to the API or sink.
    std::string sanitized = sanitize_tty_sequences(output);

    // Truncate from middle if too large - keep beginning and end (most important info)
    const size_t max_size = 20000;   // 20KB max
    const size_t head_size = 8000;   // Keep first 8KB
    const size_t tail_size = 8000;    // Keep last 8KB
    
    if (sanitized.size() > max_size) {
        if (sanitized.size() <= head_size + tail_size) {
            // §BUG-UTF8-TRUNC: cut at a codepoint boundary.
            return utf8_truncate_safe(sanitized, max_size) + "\n[Output truncated at " + std::to_string(max_size) + " bytes]";
        }
        
        // Keep head and tail, truncate middle
        // §BUG-UTF8-TRUNC: both cut points land on codepoint boundaries.
        std::string head = utf8_truncate_safe(sanitized, head_size);
        std::string tail = sanitized.substr(sanitized.size() - tail_size);
        while (!tail.empty() &&
               is_utf8_continuation_byte(static_cast<uint8_t>(tail.front())))
            tail.erase(tail.begin());
        
        // Count lines skipped in middle
        size_t lines_skipped = 0;
        size_t pos = head_size;
        while (pos < sanitized.size() - tail_size) {
            if (sanitized[pos] == '\n') {
                lines_skipped++;
            }
            pos++;
        }
        
        return head + 
            "\n\n[...] " + std::to_string(lines_skipped) + " lines skipped (" + 
            std::to_string(sanitized.size() - head_size - tail_size) + " bytes) [...]\n\n" + 
            tail +
            "\n[Output truncated - showing first " + std::to_string(head_size) + " and last " + 
            std::to_string(tail_size) + " bytes of " + std::to_string(sanitized.size()) + " total]";
    }

    return sanitized;
}

std::string BashCommandExecutor::sanitize_pkill_command(const std::string& command) {
    std::string cmd_lower = command;
    std::transform(cmd_lower.begin(), cmd_lower.end(), cmd_lower.begin(), ::tolower);
    
    // Check if this is a pkill command
    if (cmd_lower.find("pkill") != std::string::npos) {
        std::string sanitized = command;
        
        // Use regex to replace " -f" followed by space, end, or another flag
        std::regex pattern(R"( -f(?= |$|-))");
        sanitized = std::regex_replace(sanitized, pattern, " ");
        
        return sanitized;
    }
    
    return command;
}