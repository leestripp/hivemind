/**
 * @file Logger.cpp
 * @brief Implementation of the Logger class
 */

#include "Logger.h"
#include <iostream>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <regex>

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
// ============================================================================
// Logger Implementation
// ============================================================================

Logger& Logger::get_instance() {
    static Logger instance;
    return instance;
}

Logger::~Logger() {
    shutdown();
}

void Logger::initialize(const fs::path& project_root, const LoggerConfig& config) {
    std::scoped_lock lock(log_mutex_);

    // Explicit initialization wins over the cwd fallback (ensure_initialized).
    auto_initialized_ = false;

    // §logging: hardened — fs::absolute / create_directories /
    // directory_iterator can throw fs::filesystem_error. Logging is
    // best-effort: on failure stay uninitialized rather than throwing into
    // library code (callers include noexcept paths and worker threads).
    try {
        project_root_ = fs::absolute(project_root);
        config_ = config;
        enabled_ = config.enabled;

        // Create log directory
        ensure_log_directory();

        // Generate initial log file name
        current_log_path_ = get_log_directory() / generate_log_filename();

        // Cleanup old logs on startup
        cleanup_old_logs();

        session_start_ = std::chrono::system_clock::now();
        initialized_ = true;

        // Write session start marker
        std::stringstream ss;
        ss << "===========================================\n";
        ss << "hive-brain Session Started\n";
        ss << "Project: " << project_root_.string() << "\n";
        ss << "===========================================\n";
        write_to_file(ss.str());
    } catch (const std::exception&) {
        initialized_ = false;
    }
}

void Logger::ensure_initialized() {
    if (initialized_ || init_attempted_) return;  // fast path (benign race)
    {
        std::scoped_lock lock(log_mutex_);
        if (initialized_ || init_attempted_) return;  // double-checked
        init_attempted_ = true;  // one attempt only — no retry storms
    }
    // initialize() takes log_mutex_ itself, so it must be called here with
    // the lock released. fs::current_path() can throw — best-effort only.
    try {
        initialize(fs::current_path());
    } catch (const std::exception&) {
        // stay dark
    }
    std::scoped_lock lock(log_mutex_);
    auto_initialized_ = initialized_;
}

void Logger::shutdown() {
    if (!initialized_) return;
    
    std::scoped_lock lock(log_mutex_);
    
    // Write session end marker
    auto duration = std::chrono::system_clock::now() - session_start_;
    auto seconds = std::chrono::duration_cast<std::chrono::seconds>(duration).count();
    
    std::stringstream ss;
    ss << "===========================================\n";
    ss << "hive-brain Session Ended\n";
    ss << "Duration: " << seconds << " seconds\n";
    ss << "===========================================\n\n";
    write_to_file(ss.str());
    
    initialized_ = false;
}

void Logger::debug(const std::string& message) {
    log(LogLevel::DEBUG, message);
}

void Logger::info(const std::string& message) {
    log(LogLevel::INFO, message);
}

void Logger::warning(const std::string& message) {
    log(LogLevel::WARNING, message);
}

void Logger::error(const std::string& message) {
    log(LogLevel::ERROR, message);
}

void Logger::log(LogLevel level, const std::string& message) {
    ensure_initialized();
    if (!initialized_ || !enabled_) return;
    if (level < config_.level) return;
    
    std::scoped_lock lock(log_mutex_);
    
    // Check for rotation
    rotate_log_if_needed();
    
    // Format and write
    std::string entry = format_entry(level, message);
    write_to_file(entry);
    
    // Also output to stderr for ERROR level
    if (level == LogLevel::ERROR) {
        // §fix: write to stderr directly; routing through Logger would
        // re-acquire log_mutex_ (non-recursive) and self-deadlock.
        std::cerr << entry << std::flush;
    }
}

void Logger::log_with_context(LogLevel level, const std::string& message,
                               const std::vector<std::pair<std::string, std::string>>& context) {
    ensure_initialized();
    if (!initialized_ || !enabled_) return;
    if (level < config_.level) return;
    
    std::scoped_lock lock(log_mutex_);
    
    rotate_log_if_needed();
    
    std::stringstream ss;
    ss << format_entry(level, message);
    
    for (const auto& [key, value] : context) {
        ss << " | " << key << ": " << value;
    }
    
    write_to_file(ss.str() + "\n");
}

void Logger::set_level(LogLevel level) {
    std::scoped_lock lock(log_mutex_);
    config_.level = level;
}

void Logger::set_enabled(bool enabled) {
    std::scoped_lock lock(log_mutex_);
    enabled_ = enabled;
}

fs::path Logger::get_current_log_path() const {
    std::scoped_lock lock(log_mutex_);
    return current_log_path_;
}

std::vector<fs::path> Logger::get_log_files() const {
    std::vector<fs::path> log_files;
    
    if (!initialized_) return log_files;
    
    fs::path log_dir = get_log_directory();
    if (!fs::exists(log_dir)) return log_files;
    
    for (const auto& entry : fs::directory_iterator(log_dir)) {
        if (entry.is_regular_file() && entry.path().extension() == ".log") {
            log_files.push_back(entry.path());
        }
    }
    
    // Sort by name (which is chronological)
    std::sort(log_files.begin(), log_files.end());
    
    return log_files;
}

// ============================================================================
// Private Methods
// ============================================================================

void Logger::ensure_log_directory() {
    fs::path log_dir = get_log_directory();
    if (!fs::exists(log_dir)) {
        fs::create_directories(log_dir);
    }
}

void Logger::rotate_log_if_needed() {
    if (!fs::exists(current_log_path_)) return;
    
    try {
        size_t file_size = fs::file_size(current_log_path_);
        if (file_size > config_.max_file_size_bytes) {
            // Rename current log with _rotated suffix
            fs::path rotated = current_log_path_;
            rotated += ".old";
            
            // Remove old rotated file if exists
            if (fs::exists(rotated)) {
                fs::remove(rotated);
            }
            
            fs::rename(current_log_path_, rotated);
            current_log_path_ = get_log_directory() / generate_log_filename();
        }
    } catch (const fs::filesystem_error& e) {
        // §fix: stderr directly; callers hold log_mutex_ (see log()),
        // so LOG_CERR would self-deadlock on the non-recursive mutex.
        std::cerr << "Logger: Failed to rotate log: " << e.what() << std::endl;
    }
}

void Logger::cleanup_old_logs() {
    fs::path log_dir = get_log_directory();
    if (!fs::exists(log_dir)) return;
    
    auto now = std::chrono::system_clock::now();
    auto cutoff = now - std::chrono::hours(24 * config_.retention_days);
    
    try {
        for (const auto& entry : fs::directory_iterator(log_dir)) {
            if (!entry.is_regular_file()) continue;
            
            // Get file modification time
            auto mtime = fs::last_write_time(entry.path());
            // §fix: C++17-safe file_clock -> system_clock conversion;
            // file_clock ticks are not Unix seconds on common stdlibs.
            auto mtime_sys = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
                mtime - decltype(mtime)::clock::now() + std::chrono::system_clock::now()
            );
            
            // Delete if older than retention period
            if (mtime_sys < cutoff) {
                fs::remove(entry.path());
            }
        }
    } catch (const fs::filesystem_error& e) {
        // §fix: stderr directly; called from initialize() while holding
        // log_mutex_, so LOG_CERR would self-deadlock.
        std::cerr << "Logger: Failed to cleanup old logs: " << e.what() << std::endl;
    }
}

void Logger::write_to_file(const std::string& entry) {
    std::scoped_lock lock(file_mutex_);
    
    try {
        std::ofstream file(current_log_path_, std::ios::app);
        if (file.is_open()) {
            file << entry;
            file.flush();
        }
    } catch (const std::exception& e) {
        // §fix: stderr directly; LOG_CERR routes back into write_to_file()
        // which would re-acquire file_mutex_ and self-deadlock.
        std::cerr << "Logger: Failed to write to file: " << e.what() << std::endl;
    }
}

std::string Logger::format_entry(LogLevel level, const std::string& message) {
    std::stringstream ss;
    
    // Timestamp: [2026-04-07 10:30:15.123]
    ss << "[" << get_timestamp() << "] ";
    
    // Level: [INFO ]
    std::string level_str = get_level_string(level);
    ss << "[" << level_str << "] ";
    
    // Message
    ss << message << "\n";
    
    return ss.str();
}

std::string Logger::get_timestamp() {
    auto now = std::chrono::system_clock::now();
    auto time_t = std::chrono::system_clock::to_time_t(now);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()
    ) % 1000;
    
    std::tm tm_buf;
#ifdef _WIN32
    localtime_s(&tm_buf, &time_t);
#else
    localtime_r(&time_t, &tm_buf);
#endif
    
    std::stringstream ss;
    ss << std::put_time(&tm_buf, "%Y-%m-%d %H:%M:%S");
    ss << "." << std::setfill('0') << std::setw(3) << ms.count();
    
    return ss.str();
}

std::string Logger::get_level_string(LogLevel level) {
    switch (level) {
        case LogLevel::DEBUG:   return "DEBUG  ";
        case LogLevel::INFO:    return "INFO   ";
        case LogLevel::WARNING: return "WARNING";
        case LogLevel::ERROR:   return "ERROR  ";
        default:                 return "UNKNOWN";
    }
}

fs::path Logger::get_log_directory() const {
    return project_root_ / config_.log_path;
}

fs::path Logger::generate_log_filename() {
    auto now = std::chrono::system_clock::now();
    auto time_t = std::chrono::system_clock::to_time_t(now);
    
    std::tm tm_buf;
#ifdef _WIN32
    localtime_s(&tm_buf, &time_t);
#else
    localtime_r(&time_t, &tm_buf);
#endif
    
    std::stringstream ss;
    ss << std::put_time(&tm_buf, "%Y-%m-%d_%H-%M-%S");
    ss << "_session.log";
    
    return fs::path(ss.str());
}
