/**
 * @file Logger.h
 * @brief Thread-safe logging system for hive-brain
 * 
 * Logs are saved to [Project Root]/.daedalus/hive-brain/logs/ directory.
 * Supports multiple log levels, file rotation, and async logging.
 */

#pragma once

#include <string>
#include <memory>
#include <mutex>
#include <chrono>
#include <vector>
#include <functional>
#include <filesystem>

namespace fs = std::filesystem;

/**
 * @brief Log severity levels
 */
enum class LogLevel {
    DEBUG = 0,
    INFO = 1,
    WARNING = 2,
    ERROR = 3
};

/**
 * @brief Logging configuration
 */
struct LoggerConfig {
    bool enabled = true;
    std::string log_path = ".daedalus/hive-brain/logs/";  // Relative to project root
    int retention_days = 30;
    size_t max_file_size_bytes = 10 * 1024 * 1024;  // 10MB
    LogLevel level = LogLevel::INFO;
};

/**
 * @brief Logger class - singleton for global logging
 * 
 * Thread-safe logging with:
 * - Multiple log levels (DEBUG, INFO, WARNING, ERROR)
 * - Automatic daily log file rotation
 * - Async logging to avoid blocking main thread
 * - Automatic cleanup of old log files
 */
class Logger {
public:
    /**
     * @brief Get singleton instance
     */
    static Logger& get_instance();
    
    /**
     * @brief Initialize logger with project root path
     * @param project_root Path to the project root directory
     * @param config Logger configuration (optional)
     */
    void initialize(const fs::path& project_root, const LoggerConfig& config = LoggerConfig());
    
    /**
     * @brief Check if logger is initialized
     */
    bool is_initialized() const { return initialized_; }

    /**
     * @brief True when the logger self-initialized with the cwd fallback
     *        (first LOG_* call before any explicit initialize()). Hosts can
     *        use this to re-initialize with a project-scoped path.
     */
    bool is_auto_initialized() const { return auto_initialized_; }
    
    /**
     * @brief Shutdown logger gracefully
     */
    void shutdown();
    
    // Log methods
    void debug(const std::string& message);
    void info(const std::string& message);
    void warning(const std::string& message);
    void error(const std::string& message);
    
    /**
     * @brief Log with format-like interface
     * @param level Log level
     * @param message Log message
     */
    void log(LogLevel level, const std::string& message);
    
    /**
     * @brief Log with context (key=value pairs)
     * @param level Log level
     * @param message Main message
     * @param context Vector of key=value pairs
     */
    void log_with_context(LogLevel level, const std::string& message, 
                          const std::vector<std::pair<std::string, std::string>>& context);
    
    /**
     * @brief Set log level at runtime
     */
    void set_level(LogLevel level);
    
    /**
     * @brief Enable/disable logging
     */
    void set_enabled(bool enabled);
    
    /**
     * @brief Get current log file path
     */
    fs::path get_current_log_path() const;
    
    /**
     * @brief Get all log files for this project
     */
    std::vector<fs::path> get_log_files() const;
    
    // Prevent copying
    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;

private:
    Logger() = default;
    ~Logger();

    // §logging: best-effort self-initialization with the cwd fallback, so a
    // bare LOG_* call before any explicit initialize() still lands on disk.
    // Called from log()/log_with_context() WITHOUT log_mutex_ held.
    void ensure_initialized();

    void ensure_log_directory();
    void rotate_log_if_needed();
    void cleanup_old_logs();
    void write_to_file(const std::string& entry);
    std::string format_entry(LogLevel level, const std::string& message);
    std::string get_timestamp();
    std::string get_level_string(LogLevel level);
    fs::path get_log_directory() const;
    fs::path generate_log_filename();
    
    bool initialized_ = false;
    bool enabled_ = true;
    bool init_attempted_ = false;     // lazy self-init happens at most once
    bool auto_initialized_ = false;   // set when ensure_initialized() succeeded
    fs::path project_root_;
    fs::path current_log_path_;
    LoggerConfig config_;
    
    mutable std::mutex log_mutex_;
    mutable std::mutex file_mutex_;
    
    std::chrono::system_clock::time_point session_start_;
};

/**
 * @brief Convenience macros for logging
 */
#define LOG_DEBUG(msg) Logger::get_instance().debug(msg)
#define LOG_INFO(msg) Logger::get_instance().info(msg)
#define LOG_WARNING(msg) Logger::get_instance().warning(msg)
#define LOG_ERROR(msg) Logger::get_instance().error(msg)

// Context logging macro
#define LOG_WITH_CONTEXT(level, msg, ...) \
    do { \
        std::vector<std::pair<std::string, std::string>> ctx = { __VA_ARGS__ }; \
        Logger::get_instance().log_with_context(level, msg, ctx); \
    } while(0)
