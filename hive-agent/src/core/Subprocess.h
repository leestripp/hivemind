/**
 * @file Subprocess.h
 * @brief Subprocess execution with proper isolation
 * 
 * Provides a safe way to execute external commands with:
 * - Process isolation via fork()+exec()
 * - Separate process group for crash containment
 * - Pipe-based stdout/stderr capture
 * - Timeout support
 * - Proper child reaping
 */

#ifndef SUBPROCESS_H
#define SUBPROCESS_H

#include <functional>
#include <string>
#include <string_view>
#include <vector>
#include <cstdint>

/**
 * Result of subprocess execution
 */
struct SubprocessResult {
    /** Standard output (stdout) */
    std::string stdout;
    
    /** Standard error (stderr) */
    std::string stderr;
    
    /** Exit code (only valid if !signaled) */
    int exit_code;
    
    /** True if process was killed by signal */
    bool signaled;
    
    /** Signal number (only valid if signaled) */
    int signal;
    
    /** True if process timed out */
    bool timed_out;
    
    SubprocessResult() : exit_code(0), signaled(false), signal(0), timed_out(false) {}
};

/**
 * Subprocess execution class
 * 
 * Provides isolated subprocess execution using fork()+exec() pattern.
 * Each subprocess runs in its own process group, providing isolation
 * from crashes and allowing clean termination of entire process trees.
 */
class Subprocess {
public:
    /**
     * Spawn a subprocess and return immediately
     * 
     * @param args Command and arguments (args[0] is the command)
     * @param cwd Working directory for the subprocess
     * @param capture_output If true, capture stdout/stderr to result
     * @return PID of spawned process, or -1 on error
     */
    static int64_t spawn(const std::vector<std::string>& args,
                         const std::string& cwd = "",
                         bool capture_output = true);
    
    /**
     * Wait for subprocess to complete with timeout
     * 
     * @param pid Process ID from spawn()
     * @param timeout_ms Timeout in milliseconds (-1 for infinite)
     * @return true if process exited, false on timeout or error
     */
    static bool wait(int64_t pid, int timeout_ms);
    
    /**
     * Wait for subprocess and get result
     * 
     * @param pid Process ID from spawn()
     * @param timeout_ms Timeout in milliseconds (-1 for infinite)
     * @param result Output result with stdout/stderr/exit info
     * @return true if process exited normally or timed out
     */
    static bool wait_with_result(int64_t pid, int timeout_ms, SubprocessResult& result);
    
    /**
     * Kill a process and all its children (entire process group)
     * 
     * @param pid Process ID
     * @return true if signal was sent successfully
     */
    static bool kill_group(int64_t pid);
    
    /**
     * Execute a command with timeout and return result
     * 
     * This is a convenience function that spawns, waits, and returns
     * the complete result in one call.
     * 
     * @param command Command string to execute (will be parsed to argv)
     * @param cwd Working directory
     * @param timeout_ms Timeout in milliseconds
     * @return SubprocessResult with stdout, stderr, exit_code, etc.
     */
    static SubprocessResult execute(const std::string& command,
                                     const std::string& cwd = "",
                                     int timeout_ms = 120000);

    /**
     * Execute a command with timeout, invoking a callback for each chunk of
     * output as it arrives. The callback receives raw stdout/stderr bytes
     * (already combined if using execute_combined). The complete output is
     * still returned in the result.
     */
    using OutputChunkFn = std::function<void(std::string_view chunk)>;

    static SubprocessResult execute(const std::string& command,
                                     const std::string& cwd,
                                     int timeout_ms,
                                     OutputChunkFn output_chunk_cb);
    
    /**
     * Execute a command with timeout, combining stdout and stderr
     * 
     * @param command Command string
     * @param cwd Working directory
     * @param timeout_ms Timeout in milliseconds
     * @return SubprocessResult with combined output in stdout
     */
    static SubprocessResult execute_combined(const std::string& command,
                                              const std::string& cwd = "",
                                              int timeout_ms = 120000);

    /**
     * Execute a command with timeout, combining stdout and stderr, and
     * invoke a callback for each output chunk as it arrives.
     */
    static SubprocessResult execute_combined(const std::string& command,
                                              const std::string& cwd,
                                              int timeout_ms,
                                              OutputChunkFn output_chunk_cb);
    
    /**
     * Parse a shell command string into argument vector
     * 
     * Handles basic quoting but not full shell parsing.
     * 
     * @param command Shell command string
     * @return Vector of arguments
     */
    static std::vector<std::string> parse_command(const std::string& command);
    
    /**
     * Execute a shell command string (using /bin/sh -c)
     * 
     * @param command Shell command string
     * @param cwd Working directory
     * @param timeout_ms Timeout in milliseconds
     * @return SubprocessResult with stdout, stderr, exit_code
     */
    static SubprocessResult execute_shell(const std::string& command,
                                          const std::string& cwd = "",
                                          int timeout_ms = 120000);

private:
    // Internal structure to hold state during execution
    struct ExecutionState;
    
    // Internal spawn that returns pipes for reading output
    static int64_t spawn_with_pipes(const std::vector<std::string>& args,
                                     const std::string& cwd,
                                     int pipe_stdout[2],
                                     int pipe_stderr[2]);
    
    // Read from a pipe into a string (non-blocking). Returns the number of
    // bytes read, or 0 if nothing was read / EOF already reached.
    static ssize_t read_from_pipe(int pipe_fd, std::string& output, bool& eof);
    
    // Close pipe and set to -1
    static void close_pipe(int& fd);
};

#endif // SUBPROCESS_H
