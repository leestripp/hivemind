#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <optional>
#include <chrono>
#include <filesystem>
#include <ctime>
#include <nlohmann/json.hpp>

// Forward declarations

/**
 * MemoryEntry - Single piece of remembered information
 */
struct MemoryEntry {
    std::string id;                    // Unique ID (e.g., "err_001", "bld_001")
    std::string category;              // "builds", "errors", "preferences", "patterns", "sessions"
    std::string content;               // The actual content
    std::unordered_map<std::string, std::string> metadata;
    int confidence = 3;                // 1-5 scale
    std::chrono::system_clock::time_point created_at;
    std::chrono::system_clock::time_point last_accessed;
    int access_count = 0;
    std::vector<std::string> tags;     // Optional tags for filtering
    
    // JSON serialization
    nlohmann::json to_json() const;
    static MemoryEntry from_json(const nlohmann::json& j);
    
    std::string to_string() const;     // For context injection
};

/**
 * BuildEntry - Working build command record
 */
struct BuildEntry {
    std::string id;
    std::string command;
    bool success = true;
    int attempts = 1;
    std::chrono::system_clock::time_point first_used;
    std::chrono::system_clock::time_point last_used;
    int use_count = 1;
    std::string notes;
    
    nlohmann::json to_json() const;
    static BuildEntry from_json(const nlohmann::json& j);
};

/**
 * ErrorEntry - Error pattern with solution
 */
struct ErrorEntry {
    std::string id;
    std::string pattern;               // Regex or substring pattern
    std::string solution;              // Known solution (empty if unresolved)
    int occurrences = 1;
    std::chrono::system_clock::time_point first_seen;
    std::chrono::system_clock::time_point last_seen;
    bool resolved = false;
    int confidence = 2;                // Starts low, increases when resolved
    
    nlohmann::json to_json() const;
    static ErrorEntry from_json(const nlohmann::json& j);
};

/**
 * PreferenceEntry - User preference record
 */
struct PreferenceEntry {
    std::string id;
    std::string key;                   // Preference name
    std::string value;                // Preference value
    std::string inferred_from;        // How this was inferred
    std::chrono::system_clock::time_point first_observed;
    int observation_count = 1;
    bool user_confirmed = false;
    
    nlohmann::json to_json() const;
    static PreferenceEntry from_json(const nlohmann::json& j);
};

/**
 * PatternEntry - Code/project pattern
 */
struct PatternEntry {
    std::string id;
    std::string type;                 // "file_location", "naming", "style", etc.
    std::string description;
    std::string example;
    int confidence = 2;               // Starts low, increases with verification
    
    nlohmann::json to_json() const;
    static PatternEntry from_json(const nlohmann::json& j);
};

/**
 * SessionSummary - Brief session recap
 */
struct SessionSummary {
    std::string id;
    std::string summary;
    std::chrono::system_clock::time_point timestamp;
    std::vector<std::string> files_touched;
    int iterations = 0;
    
    nlohmann::json to_json() const;
    static SessionSummary from_json(const nlohmann::json& j);
};

/**
 * ProjectInfo - Cached project metadata
 */
struct ProjectInfo {
    std::string type;                  // "cmake", "makefile", "python", etc.
    std::string build_command;
    std::string test_command;
    std::vector<std::string> main_files;
    std::chrono::system_clock::time_point detected_at;
    std::chrono::system_clock::time_point last_verified;
    
    nlohmann::json to_json() const;
    static ProjectInfo from_json(const nlohmann::json& j);
};

/**
 * SessionMemory - Manages persistent context across sessions
 */
class SessionMemory {
private:
    std::filesystem::path memory_file_path_;
    std::filesystem::path memory_dir_path_;
    std::filesystem::path data_dir_;        // §Phase 8.8: per-agent namespace dir
    std::string session_id_ = "default";    // §Plan.md v0.3.5: SQLite mirror key
    
    std::string version_ = "1.0";
    std::chrono::system_clock::time_point updated_at_;
    
    // Memory categories
    std::vector<BuildEntry> builds_;
    std::vector<ErrorEntry> errors_;
    std::vector<PreferenceEntry> preferences_;
    std::vector<PatternEntry> patterns_;
    std::vector<SessionSummary> sessions_;
    ProjectInfo project_info_;
    
    // Configuration
public:
    struct Config {
        bool enabled = true;
        int max_total_size_kb = 100;
        int max_entries_per_category = 50;
        int max_session_history = 20;
        int max_entry_size_bytes = 2048;
        int memory_age_days = 30;
        bool auto_detect_errors = true;
        bool auto_detect_preferences = true;
        bool auto_detect_patterns = true;
        bool auto_learn_builds = true;
    };
private:
    Config config_;
    
    // Private methods
    void load();
    void save();
    std::string generate_id(const std::string& category);
    void consolidate();
    void age_memories();
    void ensure_memory_dir();
    size_t calculate_size() const;
    
    // Security: Filter sensitive data before storing
    std::string filter_sensitive_data(const std::string& content) const;
    std::string get_context_string() const;
    nlohmann::json to_json_full() const;
    
    // Backup corrupted memory file and reset to fresh state
    void backup_and_reset();
    
    // Memory operations helpers
    void add_build(const std::string& command, bool success);
    void add_error(const std::string& error, const std::string& solution, bool resolved);
    void update_error_resolution(const std::string& pattern, const std::string& solution);
    
public:
    /**
     * @brief Construct SessionMemory with a per-agent data directory
     *        (§Phase 8.8 / §Phase 16). SessionMemory stores its memory
     *        file at `<data_dir>/memory/memory.json` so it shares the
     *        namespace with Messages.
     *
     * AgentImpl::initialize() resolves the namespace via
     * `resolve_agent_dir(project_root, agent_name)` and passes the
     * resulting directory in. SessionMemory itself doesn't know
     * about agent_name.
     */
    SessionMemory(std::filesystem::path data_dir);
    ~SessionMemory();
    
    // Session lifecycle
    void on_session_start();
    void on_session_end(const std::string& session_summary, 
                        const std::vector<std::string>& files_touched = {},
                        int iterations = 0);
    
    // Event hooks (called from MiniMaxClient)
    void on_build_complete(const std::string& command, bool success);
    void on_error(const std::string& error, const std::string& attempted_fix = "");
    void on_error_resolved(const std::string& error, const std::string& solution);
    void on_user_correction(const std::string& context, const std::string& correction);
    void on_file_created(const std::filesystem::path& file);
    
    // Explicit memory operations
    void remember(const std::string& category, const std::string& content, 
                  int confidence = 3, const std::vector<std::string>& tags = {});
    bool forget(const std::string& id);
    
    // Query methods
    std::vector<const BuildEntry*> get_builds() const;
    std::vector<const ErrorEntry*> get_errors() const;
    std::vector<const ErrorEntry*> get_unresolved_errors() const;
    std::vector<const PreferenceEntry*> get_preferences() const;
    std::vector<const PatternEntry*> get_patterns() const;
    std::vector<const SessionSummary*> get_recent_sessions(int count = 5) const;
    
    // Search
    std::optional<const ErrorEntry*> find_error(const std::string& error_pattern) const;
    std::optional<const BuildEntry*> find_build(const std::string& command) const;
    
    // Context for system prompt
    std::string get_system_prompt_addition() const;
    
    // Statistics
    struct Stats {
        int total_builds;
        int total_errors;
        int resolved_errors;
        int total_preferences;
        int total_patterns;
        int total_sessions;
        size_t total_size_bytes;
        std::chrono::system_clock::time_point last_updated;
    };
    Stats get_stats() const;
    
    // Configuration
    Config get_config() const { return config_; }
    void set_config(const Config& config) { config_ = config; }
    
    // CLI support
    void clear_all_memories();
    bool is_enabled() const { return config_.enabled; }
    
    // Testing support
    void reset_for_testing();
};
