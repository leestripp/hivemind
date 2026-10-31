/**
 * @file BackgroundProcessManager.h
 * @brief Background process management for server applications
 *
 * Provides ability to:
 * - Start processes in background
 * - Send input to processes
 * - Poll output from processes
 * - Stop background processes
 * - List active processes
 *
 * Each background process is tracked by a unique process_id (e.g., "bg_001")
 */

#ifndef BACKGROUND_PROCESS_MANAGER_H
#define BACKGROUND_PROCESS_MANAGER_H

#include <string>
#include <vector>
#include <unordered_map>
#include <optional>
#include <chrono>
#include <filesystem>
#include <ctime>
#include <cstdint>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>

/**
 * Information about a running background process
 */
struct BackgroundProcessInfo {
    std::string process_id;           // Unique ID assigned by manager (e.g., "bg_001")
    pid_t pid;                        // System PID
    std::string command;              // Original command
    std::string working_dir;          // Working directory at start
    std::chrono::system_clock::time_point started_at;
    bool exited = false;              // True if process has exited
    int exit_code = 0;               // Exit code if exited
    bool signaled = false;            // True if killed by signal
    int signal_num = 0;               // Signal number if signaled
    std::string fifo_path;            // Path to FIFO for stdin
    std::string stdout_path;          // Path to stdout capture file
    std::string stderr_path;          // Path to stderr capture file
    std::string origin_task_id;       // Turn that started the process
    std::string origin_thread_id;     // Thread to notify on completion
    bool completion_notified = false; // Exactly-once automatic notification
};

/**
 * Result of a send operation
 */
struct SendResult {
    bool success = false;
    std::string error;
    size_t bytes_sent = 0;
};

/**
 * Output from a background process
 */
struct ProcessOutput {
    std::string stdout;
    std::string stderr;
    bool exited = false;
    int exit_code = 0;
    bool signaled = false;
    int signal_num = 0;
};

/**
 * BackgroundProcessManager - Manages background processes for the agent
 *
 * This singleton class allows the agent to:
 * 1. Start server applications in background
 * 2. Continue its reasoning loop while server runs
 * 3. Send commands to the server
 * 4. Read output from the server
 * 5. Stop the server when done
 *
 * Thread safety: public methods are serialized internally. A single watcher
 * detects natural exits and invokes the optional completion callback.
 */
class BackgroundProcessManager {
public:
    /**
     * Get singleton instance
     */
    static BackgroundProcessManager& get_instance();

    /**
     * Start a process in background
     *
     * @param command Command to execute (will be parsed and run)
     * @param cwd Working directory for the process
     * @return Process ID on success, empty string on failure
     */
    std::string start_background(const std::string& command, const std::string& cwd = "");

    /// Set the turn/thread associated with subsequently started processes.
    void set_current_origin(std::string task_id, std::string thread_id);

    using CompletionCallback = std::function<void(
        const BackgroundProcessInfo&, const ProcessOutput&)>;

    /// Called once when a background process exits naturally. Explicit
    /// stop_process()/cleanup_all() do not notify; polling an exited process
    /// consumes its completion without an automatic callback.
    void set_completion_callback(CompletionCallback callback);

    /// Stop the watcher and all processes. Idempotent; safe before static
    /// destruction so callbacks never outlive their host's queue/state.
    void shutdown();

    /**
     * Send input to a background process's stdin
     *
     * @param process_id ID returned from start_background()
     * @param input Data to send
     * @return SendResult with success/error info
     */
    SendResult send_to_process(const std::string& process_id, const std::string& input);

    /**
     * Poll output from a background process
     *
     * @param process_id ID returned from start_background()
     * @param clear_after_read If true, clear the output files after reading
     * @return ProcessOutput with stdout/stderr content
     */
    ProcessOutput poll_output(const std::string& process_id, bool clear_after_read = false);

    /**
     * Stop a background process
     *
     * @param process_id ID returned from start_background()
     * @return true if process was stopped (or already dead)
     */
    bool stop_process(const std::string& process_id);

    /**
     * List all active (not exited) background processes
     *
     * @return Vector of process info for active processes
     */
    std::vector<BackgroundProcessInfo> list_active_processes() const;

    /**
     * Get info about a specific process
     *
     * @param process_id ID returned from start_background()
     * @return Process info if found, nullopt if not found
     */
    std::optional<BackgroundProcessInfo> get_process_info(const std::string& process_id);

    /**
     * Clean up all resources (called on exit)
     *
     * Kills all running processes and removes FIFO/buffer files.
     */
    void cleanup_all();

    /**
     * Check if a process has exited
     *
     * @param process_id ID returned from start_background()
     * @return true if process has exited
     */
    bool has_exited(const std::string& process_id);

    /**
     * Get the base temp directory for process files
     */
    const std::filesystem::path& get_temp_base() const { return temp_base_; }

private:
    // Singleton - private constructors
    BackgroundProcessManager();
    ~BackgroundProcessManager();
    BackgroundProcessManager(const BackgroundProcessManager&) = delete;
    BackgroundProcessManager& operator=(const BackgroundProcessManager&) = delete;

    // Internal methods
    std::string generate_process_id();
    bool create_fifo(const std::string& path);
    bool remove_fifo(const std::string& path);
    void ensure_temp_dir();
    void cleanup_process(const std::string& process_id);
    pid_t spawn_process(const std::string& command, const std::string& cwd,
                        const std::string& fifo_path,
                        const std::string& stdout_path,
                        const std::string& stderr_path);
    bool write_to_fifo(const std::string& fifo_path, const std::string& data);
    std::string read_file(const std::string& path);
    void clear_file(const std::string& path);
    bool is_process_running(pid_t pid) const;
    bool kill_process_group(pid_t pid);
    bool refresh_process_status(BackgroundProcessInfo& info);
    void watch_processes(std::stop_token stop_token);

    // Configuration
    static constexpr size_t MAX_OUTPUT_SIZE = 100000;  // 100KB max output buffer
    static constexpr const char* TEMP_DIR_PREFIX = "hive_bg_";

    // State
    std::filesystem::path temp_base_;
    std::unordered_map<std::string, BackgroundProcessInfo> processes_;
    int process_counter_ = 0;
    std::string current_task_id_;
    std::string current_thread_id_;
    CompletionCallback completion_callback_;
    mutable std::mutex mutex_;
    std::condition_variable_any watcher_cv_;
    std::jthread watcher_;
};

#endif // BACKGROUND_PROCESS_MANAGER_H