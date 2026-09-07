#include "SessionMemory.h"
#include "core/settings.h"   // v0.3.5: SettingsStore mirror
#include "Logger.h"
#include "core/Utf8Utils.h"  // §B11/B28: shared utf8_sanitize_replace_invalid

namespace {

// §BUG-MEMORY-BLOAT (2026-08-10): one-line, length-capped rendering of a
// stored shell command for prompt dumps. Multi-line heredoc scripts
// collapse to their first line — the full text stays in the store.
// Unbounded dumps of every build ever recorded (multi-KB heredocs
// included) inflated the system prompt to ~250k tokens on long-lived
// projects, pinning the context over the 90% auto-compress threshold
// with nothing compressible.
std::string truncate_memory_command(const std::string& cmd,
                                    size_t max_len = 200) {
    std::string out = cmd;
    std::replace(out.begin(), out.end(), '\n', ' ');
    std::replace(out.begin(), out.end(), '\r', ' ');
    std::replace(out.begin(), out.end(), '\t', ' ');
    // §BUG-UTF8-TRUNC: cut at a codepoint boundary — a byte-level resize
    // can split a multi-byte UTF-8 sequence, and the invalid string later
    // kills nlohmann dump/parse with type_error.316.
    out = utf8_truncate_safe(out, max_len);
    if (out.size() < cmd.size()) out += "...";
    return out;
}

// §BUG-MEMORY-BLOAT: evict the least useful build entries (lowest
// use_count, then oldest last_used) until the category fits its cap.
// builds_ was the ONLY category never pruned — consolidate() trimmed
// patterns/sessions but builds grew unbounded.
void prune_build_entries(std::vector<BuildEntry>& builds, int max_entries) {
    while (static_cast<int>(builds.size()) > max_entries) {
        auto worst = std::min_element(builds.begin(), builds.end(),
            [](const BuildEntry& a, const BuildEntry& b) {
                if (a.use_count != b.use_count) return a.use_count < b.use_count;
                return a.last_used < b.last_used;
            });
        builds.erase(worst);
    }
}

} // namespace
#include <fstream>
#include <iostream>
#include <algorithm>
#include <sstream>
#include <iomanip>
#include <regex>
#include <random>
#include <numeric>
#include <ctime>       // ::timegm / ::_mkgmtime

#include "core/Logger.h"

// §Phase 5.1: helper macro that converts LOG_CERR(INFO,  ...); style
// statements into LOG_INFO/LOG_WARNING/LOG_ERROR calls via the
// existing Logger. Same formatted text routed through the Logger
// instead of writing to stderr directly.
#define LOG_CERR(level, expr) do {                          \
        std::ostringstream _hive_msg;                   \
        _hive_msg << expr;                               \
        LOG_##level(_hive_msg.str());                    \
    } while (0)
using json = nlohmann::json;

// ============================================================================
// UTF-8 Sanitization Helpers (to prevent memory file corruption)
// §B11/B28: uses shared utf8_sanitize_replace_invalid from Utf8Utils.h
// ============================================================================

// Recursively sanitize string values in JSON to ensure valid UTF-8
static void sanitize_json(json& j) {
    if (j.is_object()) {
        for (auto& [key, value] : j.items()) {
            if (value.is_string()) {
                value = utf8_sanitize_replace_invalid(value.get<std::string>());
            } else if (value.is_object() || value.is_array()) {
                sanitize_json(value);
            }
        }
    } else if (j.is_array()) {
        for (auto& item : j) {
            if (item.is_string()) {
                item = utf8_sanitize_replace_invalid(item.get<std::string>());
            } else if (item.is_object() || item.is_array()) {
                sanitize_json(item);
            }
        }
    }
}

// ============================================================================
// Helper Functions
// ============================================================================

static std::string time_point_to_iso(const std::chrono::system_clock::time_point& tp) {
    auto time = std::chrono::system_clock::to_time_t(tp);
    std::stringstream ss;
    ss << std::put_time(std::gmtime(&time), "%Y-%m-%dT%H:%M:%SZ");
    return ss.str();
}

static std::chrono::system_clock::time_point iso_to_time_point(const std::string& iso) {
    std::tm tm = {};
    std::stringstream ss(iso);
    ss >> std::get_time(&tm, "%Y-%m-%dT%H:%M:%SZ");
    if (ss.fail()) {
        return std::chrono::system_clock::now();
    }
    // §fix (2026-07-18): std::mktime interprets tm as LOCAL time, but
    // time_point_to_iso() writes UTC ('Z') — persisted timestamps
    // round-tripped shifted by the host's UTC offset. Use timegm (UTC).
#ifdef _WIN32
    return std::chrono::system_clock::from_time_t(::_mkgmtime(&tm));
#else
    return std::chrono::system_clock::from_time_t(::timegm(&tm));
#endif
}

static std::string generate_uuid() {
    static std::random_device rd;
    static std::mt19937 gen(rd());
    static std::uniform_int_distribution<> dis(0, 15);
    static std::uniform_int_distribution<> dis2(8, 11);
    
    std::stringstream ss;
    ss << std::hex;
    for (int i = 0; i < 8; i++) ss << dis(gen);
    ss << "-";
    for (int i = 0; i < 4; i++) ss << dis(gen);
    ss << "-4";  // Version 4
    for (int i = 0; i < 3; i++) ss << dis(gen);
    ss << "-";
    ss << dis2(gen);
    for (int i = 0; i < 3; i++) ss << dis(gen);
    ss << "-";
    for (int i = 0; i < 12; i++) ss << dis(gen);
    return ss.str();
}

// ============================================================================
// MemoryEntry Implementation
// ============================================================================

nlohmann::json MemoryEntry::to_json() const {
    json j;
    j["id"] = id;
    j["category"] = category;
    j["content"] = content;
    j["metadata"] = metadata;
    j["confidence"] = confidence;
    j["created_at"] = time_point_to_iso(created_at);
    j["last_accessed"] = time_point_to_iso(last_accessed);
    j["access_count"] = access_count;
    j["tags"] = tags;
    return j;
}

MemoryEntry MemoryEntry::from_json(const nlohmann::json& j) {
    MemoryEntry entry;
    entry.id = j.value("id", "");
    entry.category = j.value("category", "");
    entry.content = j.value("content", "");
    entry.metadata = j.value("metadata", std::unordered_map<std::string, std::string>{});
    entry.confidence = j.value("confidence", 3);
    entry.created_at = iso_to_time_point(j.value("created_at", ""));
    entry.last_accessed = iso_to_time_point(j.value("last_accessed", ""));
    entry.access_count = j.value("access_count", 0);
    entry.tags = j.value("tags", std::vector<std::string>{});
    return entry;
}

std::string MemoryEntry::to_string() const {
    return "[" + category + "] " + content;
}

// ============================================================================
// BuildEntry Implementation
// ============================================================================

json BuildEntry::to_json() const {
    json j;
    j["id"] = id;
    j["command"] = command;
    j["success"] = success;
    j["attempts"] = attempts;
    j["first_used"] = time_point_to_iso(first_used);
    j["last_used"] = time_point_to_iso(last_used);
    j["use_count"] = use_count;
    j["notes"] = notes;
    return j;
}

BuildEntry BuildEntry::from_json(const nlohmann::json& j) {
    BuildEntry entry;
    entry.id = j.value("id", "");
    entry.command = j.value("command", "");
    entry.success = j.value("success", true);
    entry.attempts = j.value("attempts", 1);
    entry.first_used = iso_to_time_point(j.value("first_used", ""));
    entry.last_used = iso_to_time_point(j.value("last_used", ""));
    entry.use_count = j.value("use_count", 1);
    entry.notes = j.value("notes", "");
    return entry;
}

// ============================================================================
// ErrorEntry Implementation
// ============================================================================

json ErrorEntry::to_json() const {
    json j;
    j["id"] = id;
    j["pattern"] = pattern;
    j["solution"] = solution;
    j["occurrences"] = occurrences;
    j["first_seen"] = time_point_to_iso(first_seen);
    j["last_seen"] = time_point_to_iso(last_seen);
    j["resolved"] = resolved;
    j["confidence"] = confidence;
    return j;
}

ErrorEntry ErrorEntry::from_json(const nlohmann::json& j) {
    ErrorEntry entry;
    entry.id = j.value("id", "");
    entry.pattern = j.value("pattern", "");
    entry.solution = j.value("solution", "");
    entry.occurrences = j.value("occurrences", 1);
    entry.first_seen = iso_to_time_point(j.value("first_seen", ""));
    entry.last_seen = iso_to_time_point(j.value("last_seen", ""));
    entry.resolved = j.value("resolved", false);
    entry.confidence = j.value("confidence", 2);
    return entry;
}

// ============================================================================
// PreferenceEntry Implementation
// ============================================================================

json PreferenceEntry::to_json() const {
    json j;
    j["id"] = id;
    j["key"] = key;
    j["value"] = value;
    j["inferred_from"] = inferred_from;
    j["first_observed"] = time_point_to_iso(first_observed);
    j["observation_count"] = observation_count;
    j["user_confirmed"] = user_confirmed;
    return j;
}

PreferenceEntry PreferenceEntry::from_json(const nlohmann::json& j) {
    PreferenceEntry entry;
    entry.id = j.value("id", "");
    entry.key = j.value("key", "");
    entry.value = j.value("value", "");
    entry.inferred_from = j.value("inferred_from", "");
    entry.first_observed = iso_to_time_point(j.value("first_observed", ""));
    entry.observation_count = j.value("observation_count", 1);
    entry.user_confirmed = j.value("user_confirmed", false);
    return entry;
}

// ============================================================================
// PatternEntry Implementation
// ============================================================================

json PatternEntry::to_json() const {
    json j;
    j["id"] = id;
    j["type"] = type;
    j["description"] = description;
    j["example"] = example;
    j["confidence"] = confidence;
    return j;
}

PatternEntry PatternEntry::from_json(const nlohmann::json& j) {
    PatternEntry entry;
    entry.id = j.value("id", "");
    entry.type = j.value("type", "");
    entry.description = j.value("description", "");
    entry.example = j.value("example", "");
    entry.confidence = j.value("confidence", 2);
    return entry;
}

// ============================================================================
// SessionSummary Implementation
// ============================================================================

json SessionSummary::to_json() const {
    json j;
    j["id"] = id;
    j["summary"] = summary;
    j["timestamp"] = time_point_to_iso(timestamp);
    j["files_touched"] = files_touched;
    j["iterations"] = iterations;
    return j;
}

SessionSummary SessionSummary::from_json(const nlohmann::json& j) {
    SessionSummary entry;
    entry.id = j.value("id", "");
    entry.summary = j.value("summary", "");
    entry.timestamp = iso_to_time_point(j.value("timestamp", ""));
    entry.files_touched = j.value("files_touched", std::vector<std::string>{});
    entry.iterations = j.value("iterations", 0);
    return entry;
}

// ============================================================================
// ProjectInfo Implementation
// ============================================================================

json ProjectInfo::to_json() const {
    json j;
    j["type"] = type;
    j["build_command"] = build_command;
    j["test_command"] = test_command;
    j["main_files"] = main_files;
    j["detected_at"] = time_point_to_iso(detected_at);
    j["last_verified"] = time_point_to_iso(last_verified);
    return j;
}

ProjectInfo ProjectInfo::from_json(const nlohmann::json& j) {
    ProjectInfo info;
    info.type = j.value("type", "");
    info.build_command = j.value("build_command", "");
    info.test_command = j.value("test_command", "");
    info.main_files = j.value("main_files", std::vector<std::string>{});
    info.detected_at = iso_to_time_point(j.value("detected_at", ""));
    info.last_verified = iso_to_time_point(j.value("last_verified", ""));
    return info;
}

// ============================================================================
// SessionMemory Implementation
// ============================================================================

SessionMemory::SessionMemory(std::filesystem::path data_dir)
    : data_dir_(std::move(data_dir)) {

    // §Phase 8.8 / §Phase 16: data_dir is the per-agent namespace dir
    // (e.g. `<project>/.daedalus/hive-brain/` or
    // `<project>/.daedalus/daedalus-bob/`). Memory lives under the
    // `memory/` subdir.
    memory_dir_path_ = data_dir_ / "memory";
    memory_file_path_ = memory_dir_path_ / "memory.json";

    ensure_memory_dir();
    
    // §fix (2026-07-18): call load() unconditionally. load() consults
    // the SQLite mirror FIRST, so gating it on memory.json existing
    // meant the mirror was never read after backup_and_reset() renamed
    // a corrupt memory.json — the next session started empty despite a
    // good mirror. load() already handles both-sources-absent.
    load();
    if (updated_at_.time_since_epoch() == std::chrono::system_clock::duration::zero()) {
        // Initialize with defaults
        updated_at_ = std::chrono::system_clock::now();
    }
}

SessionMemory::~SessionMemory() {
    // Auto-save on destruction
    if (config_.enabled) {
        save();
    }
}

void SessionMemory::ensure_memory_dir() {
    if (!std::filesystem::exists(memory_dir_path_)) {
        std::filesystem::create_directories(memory_dir_path_);
    }
}

void SessionMemory::backup_and_reset() {
    // Rename corrupted memory file to backup
    std::filesystem::path backup_path = memory_file_path_;
    backup_path += ".corrupted." + std::to_string(
        std::chrono::system_clock::now().time_since_epoch().count());
    
    try {
        if (std::filesystem::exists(memory_file_path_)) {
            std::filesystem::rename(memory_file_path_, backup_path);
            LOG_CERR(INFO, "SessionMemory: Backed up corrupted file to " << backup_path.filename().string());
        }
    } catch (const std::exception& e) {
        LOG_CERR(ERROR, "SessionMemory: Failed to backup corrupted file: " << e.what());
        // Try to delete the corrupted file if backup failed
        try {
            std::filesystem::remove(memory_file_path_);
        } catch (...) {}
    }
    
    // Reset to fresh state
    version_ = "1.0";
    updated_at_ = std::chrono::system_clock::now();
    builds_.clear();
    errors_.clear();
    preferences_.clear();
    patterns_.clear();
    sessions_.clear();
}

size_t SessionMemory::calculate_size() const {
    std::stringstream ss;
    json j = to_json_full();
    ss << j;
    return ss.str().size();
}

void SessionMemory::load() {
    // §Plan.md v0.3.5: prefer SQLite mirror if present. Falls back to
    // the legacy JSON file at <data_dir>/memory/memory.json. The
    // SQLite mirror is a plain kv_settings row under "mem.<session_id>".
    try {
        auto store = hive::SettingsStore::for_home(memory_dir_path_);
        auto blob = store.get_session_memory(session_id_);
        if (!blob.empty()) {
            // §B22 fix: wrap blob parse in its own try/catch so corrupt
            // rows are deleted and the next load doesn't try the same bad blob.
            try {
                json j = json::parse(blob);
                // (mirror of the JSON parse body below — keep them in sync)
                version_ = j.value("version", "1.0");
                updated_at_ = iso_to_time_point(j.value("updated_at", ""));
                builds_.clear();
                if (j.contains("builds"))
                    for (const auto& b : j["builds"]) builds_.push_back(BuildEntry::from_json(b));
                errors_.clear();
                if (j.contains("errors"))
                    for (const auto& e : j["errors"]) errors_.push_back(ErrorEntry::from_json(e));
                preferences_.clear();
                if (j.contains("preferences"))
                    for (const auto& p : j["preferences"]) preferences_.push_back(PreferenceEntry::from_json(p));
                patterns_.clear();
                if (j.contains("patterns"))
                    for (const auto& p : j["patterns"]) patterns_.push_back(PatternEntry::from_json(p));
                sessions_.clear();
                if (j.contains("sessions"))
                    for (const auto& s : j["sessions"]) sessions_.push_back(SessionSummary::from_json(s));
                if (j.contains("project")) project_info_ = ProjectInfo::from_json(j["project"]);
                if (j.contains("config")) {
                    const auto& cfg = j["config"];
                    config_.enabled                = cfg.value("enabled", true);
                    config_.max_total_size_kb      = cfg.value("max_total_size_kb", 100);
                    config_.max_entries_per_category = cfg.value("max_entries_per_category", 50);
                    config_.max_session_history    = cfg.value("max_session_history", 20);
                    config_.memory_age_days        = cfg.value("memory_age_days", 30);
                    config_.auto_detect_errors     = cfg.value("auto_detect_errors", true);
                    config_.auto_detect_preferences = cfg.value("auto_detect_preferences", true);
                    config_.auto_learn_builds      = cfg.value("auto_learn_builds", true);
                }
                LOG_INFO("SessionMemory: Loaded from SQLite mirror | Builds: "
                         + std::to_string(builds_.size()));
                return;
            } catch (const json::parse_error&) {
                // Corrupt mirror: delete it and fall through to JSON (B22)
                LOG_WARNING("SessionMemory: SQLite mirror corrupt; deleting and falling back to JSON");
                store.delete_session_memory(session_id_);
            }
        }
    } catch (const std::exception& e) {
        // Mirror unavailable — fall through to JSON.
        LOG_WARNING("SessionMemory: SQLite mirror load failed ("
                    + std::string(e.what()) + "); falling back to JSON");
    }

    try {
        // Read file content into string first for better error handling
        std::ifstream file(memory_file_path_);
        if (!file.is_open()) {
            return;
        }

        // Get file size
        file.seekg(0, std::ios::end);
        size_t file_size = file.tellg();
        file.seekg(0, std::ios::beg);

        // Read entire file into string
        std::string file_content;
        file_content.resize(file_size);
        file.read(file_content.data(), file_size);
        file.close();

        if (file_content.empty()) {
            return;
        }
        
        // Parse the JSON
        json j = json::parse(file_content);
        
        version_ = j.value("version", "1.0");
        updated_at_ = iso_to_time_point(j.value("updated_at", ""));
        
        // Load builds
        builds_.clear();
        if (j.contains("builds")) {
            for (const auto& b : j["builds"]) {
                builds_.push_back(BuildEntry::from_json(b));
            }
        }
        
        // Load errors
        errors_.clear();
        if (j.contains("errors")) {
            for (const auto& e : j["errors"]) {
                errors_.push_back(ErrorEntry::from_json(e));
            }
        }
        
        // Load preferences
        preferences_.clear();
        if (j.contains("preferences")) {
            for (const auto& p : j["preferences"]) {
                preferences_.push_back(PreferenceEntry::from_json(p));
            }
        }
        
        // Load patterns
        patterns_.clear();
        if (j.contains("patterns")) {
            for (const auto& p : j["patterns"]) {
                patterns_.push_back(PatternEntry::from_json(p));
            }
        }
        
        // Load sessions
        sessions_.clear();
        if (j.contains("sessions")) {
            for (const auto& s : j["sessions"]) {
                sessions_.push_back(SessionSummary::from_json(s));
            }
        }
        
        // Load project info
        if (j.contains("project")) {
            project_info_ = ProjectInfo::from_json(j["project"]);
        }
        
        // Load config
        if (j.contains("config")) {
            const auto& cfg = j["config"];
            config_.enabled = cfg.value("enabled", true);
            config_.max_total_size_kb = cfg.value("max_total_size_kb", 100);
            config_.max_entries_per_category = cfg.value("max_entries_per_category", 50);
            config_.max_session_history = cfg.value("max_session_history", 20);
            config_.memory_age_days = cfg.value("memory_age_days", 30);
            config_.auto_detect_errors = cfg.value("auto_detect_errors", true);
            config_.auto_detect_preferences = cfg.value("auto_detect_preferences", true);
            config_.auto_learn_builds = cfg.value("auto_learn_builds", true);
        }
        
        // Log successful load
        LOG_INFO("SessionMemory: Loaded | Builds: " + std::to_string(builds_.size()) + 
                 " | Errors: " + std::to_string(errors_.size()) +
                 " | Preferences: " + std::to_string(preferences_.size()) +
                 " | Sessions: " + std::to_string(sessions_.size()));
        
    } catch (const json::parse_error& e) {
        // Corrupted memory file - backup and start fresh
        LOG_CERR(INFO, "SessionMemory: Corrupted memory file: " << e.what());
        LOG_ERROR("SessionMemory: Corrupted memory file - backing up and resetting");
        backup_and_reset();
    } catch (const json::type_error& e) {
        // Invalid data types in memory file - backup and start fresh
        LOG_CERR(INFO, "SessionMemory: Invalid data format: " << e.what());
        LOG_ERROR("SessionMemory: Invalid data format - backing up and resetting");
        backup_and_reset();
    } catch (const std::exception& e) {
        // Log error but don't crash - start fresh
        LOG_CERR(ERROR, "SessionMemory: Failed to load: " << e.what());
        LOG_ERROR("SessionMemory: Failed to load - " + std::string(e.what()));
        backup_and_reset();
    } catch (...) {
        // Catch absolutely everything to prevent crashes
        LOG_CERR(INFO, "SessionMemory: Unknown error loading memory file");
        LOG_ERROR("SessionMemory: Unknown error loading memory file");
        backup_and_reset();
    }
}

void SessionMemory::save() {
    try {
        ensure_memory_dir();

        json j = to_json_full();

        // Sanitize all string values to prevent invalid UTF-8 from corrupting the file
        sanitize_json(j);

        std::ofstream file(memory_file_path_);
        if (!file.is_open()) {
            LOG_CERR(ERROR, "SessionMemory: Failed to open file for writing");
            LOG_ERROR("SessionMemory: Failed to open file for writing");
            return;
        }

        file << std::setw(2) << j << '\n';

        LOG_DEBUG("SessionMemory: Saved | Total entries: " + std::to_string(builds_.size() + errors_.size() + preferences_.size()));

    } catch (const std::exception& e) {
        LOG_CERR(ERROR, "SessionMemory: Failed to save: " << e.what());
        LOG_ERROR("SessionMemory: Failed to save - " + std::string(e.what()));
    }

    // §Plan.md v0.3.5: write-through mirror to SQLite (best-effort).
    // Failure here must NOT prevent the JSON save above from succeeding —
    // the JSON file is still the source of truth in v0.3.5.
    try {
        json mirror = to_json_full();
        sanitize_json(mirror);
        auto store = hive::SettingsStore::for_home(memory_dir_path_);
        store.set_session_memory(session_id_, mirror.dump());
    } catch (const std::exception& e) {
        LOG_WARNING("SessionMemory: SQLite mirror save failed ("
                    + std::string(e.what()) + ")");
    }
}

json SessionMemory::to_json_full() const {
    json j;
    j["version"] = version_;
    j["updated_at"] = time_point_to_iso(updated_at_);
    
    j["builds"] = json::array();
    for (const auto& b : builds_) {
        j["builds"].push_back(b.to_json());
    }
    
    j["errors"] = json::array();
    for (const auto& e : errors_) {
        j["errors"].push_back(e.to_json());
    }
    
    j["preferences"] = json::array();
    for (const auto& p : preferences_) {
        j["preferences"].push_back(p.to_json());
    }
    
    j["patterns"] = json::array();
    for (const auto& p : patterns_) {
        j["patterns"].push_back(p.to_json());
    }
    
    j["sessions"] = json::array();
    for (const auto& s : sessions_) {
        j["sessions"].push_back(s.to_json());
    }
    
    j["project"] = project_info_.to_json();
    
    j["config"] = {
        {"enabled", config_.enabled},
        {"max_total_size_kb", config_.max_total_size_kb},
        {"max_entries_per_category", config_.max_entries_per_category},
        {"max_session_history", config_.max_session_history},
        {"memory_age_days", config_.memory_age_days},
        {"auto_detect_errors", config_.auto_detect_errors},
        {"auto_detect_preferences", config_.auto_detect_preferences},
        {"auto_learn_builds", config_.auto_learn_builds}
    };
    
    return j;
}

std::string SessionMemory::generate_id(const std::string& category) {
    std::string prefix;
    if (category == "builds") prefix = "bld";
    else if (category == "errors") prefix = "err";
    else if (category == "preferences") prefix = "pref";
    else if (category == "patterns") prefix = "pat";
    else if (category == "sessions") prefix = "ses";
    else prefix = "mem";
    
    return prefix + "_" + generate_uuid().substr(0, 8);
}

// ============================================================================
// Session Lifecycle
// ============================================================================

void SessionMemory::on_session_start() {
    if (!config_.enabled) return;
    
    // Load memory if not already loaded
    if (builds_.empty() && errors_.empty() && preferences_.empty()) {
        load();
    }
    
    // Age old memories
    age_memories();
    
    // Update project info timestamp
    project_info_.last_verified = std::chrono::system_clock::now();
    
    LOG_INFO("SessionMemory: Session started | Loaded entries: " + 
             std::to_string(builds_.size() + errors_.size() + preferences_.size()));
}

void SessionMemory::on_session_end(const std::string& session_summary,
                                   const std::vector<std::string>& files_touched,
                                   int iterations) {
    if (!config_.enabled) return;
    
    // Add session summary
    SessionSummary summary;
    summary.id = generate_id("sessions");
    summary.summary = session_summary;
    summary.timestamp = std::chrono::system_clock::now();
    summary.files_touched = files_touched;
    summary.iterations = iterations;
    sessions_.push_back(summary);
    
    LOG_INFO("SessionMemory: Session ended | Summary: " + session_summary.substr(0, 50) + 
             " | Files: " + std::to_string(files_touched.size()) +
             " | Iterations: " + std::to_string(iterations));
    
    // Consolidate memories
    consolidate();
    
    // Save
    updated_at_ = std::chrono::system_clock::now();
    save();
}

// ============================================================================
// Event Hooks
// ============================================================================

void SessionMemory::on_build_complete(const std::string& command, bool success) {
    if (!config_.enabled || !config_.auto_learn_builds) return;
    
    add_build(command, success);
    
    // Check if this resolves any previous errors
    if (success) {
        for (auto& error : errors_) {
            if (!error.resolved) {
                // If the build succeeded after an error, the error might be resolved
                // But we don't auto-resolve - the user or agent should confirm
            }
        }
    }
    
    save();
}

void SessionMemory::add_build(const std::string& command, bool success) {
    // Filter sensitive data before storing
    std::string filtered_command = filter_sensitive_data(command);

    // §BUG-MEMORY-BLOAT: enforce the configured entry cap at store time —
    // multi-KB heredoc scripts are useless as memory entries and were the
    // bulk of a ~3 MB memory store that bloated the system prompt.
    const size_t max_entry = static_cast<size_t>(config_.max_entry_size_bytes);
    if (filtered_command.size() > max_entry) {
        // §BUG-UTF8-TRUNC: never split a codepoint — the corrupted entry
        // escaped into tool results (memory recall) and aborted the turn
        // via type_error.316 on the next payload dump.
        filtered_command =
            utf8_truncate_safe(filtered_command, max_entry) + "...[truncated]";
    }

    // Check if this command already exists
    for (auto& build : builds_) {
        if (build.command == filtered_command) {
            build.use_count++;
            build.last_used = std::chrono::system_clock::now();
            build.success = success;  // Update success status
            return;
        }
    }
    
    // Add new build entry
    BuildEntry entry;
    entry.id = generate_id("builds");
    entry.command = filtered_command;
    entry.success = success;
    entry.first_used = std::chrono::system_clock::now();
    entry.last_used = std::chrono::system_clock::now();
    entry.use_count = 1;
    
    builds_.push_back(entry);
    // §BUG-MEMORY-BLOAT: keep the category inside its configured cap.
    prune_build_entries(builds_, config_.max_entries_per_category);
    LOG_INFO("Memory: New build recorded | Success: " + std::string(success ? "yes" : "no"));
}

void SessionMemory::on_error(const std::string& error, const std::string& attempted_fix) {
    if (!config_.enabled || !config_.auto_detect_errors) return;
    
    add_error(error, attempted_fix, false);
    save();
}

void SessionMemory::add_error(const std::string& error, const std::string& solution, bool resolved) {
    // Filter sensitive data before storing
    std::string filtered_error = filter_sensitive_data(error);
    std::string filtered_solution = filter_sensitive_data(solution);
    
    // Check if this error pattern already exists
    for (auto& err : errors_) {
        if (err.pattern == filtered_error) {
            err.occurrences++;
            err.last_seen = std::chrono::system_clock::now();
            if (!filtered_solution.empty()) {
                err.solution = filtered_solution;
            }
            return;
        }
    }
    
    // Add new error entry
    ErrorEntry entry;
    entry.id = generate_id("errors");
    entry.pattern = filtered_error;
    entry.solution = filtered_solution;
    entry.occurrences = 1;
    entry.first_seen = std::chrono::system_clock::now();
    entry.last_seen = std::chrono::system_clock::now();
    entry.resolved = resolved;
    entry.confidence = resolved ? 4 : 2;
    
    errors_.push_back(entry);
    LOG_INFO("Memory: New error recorded | Resolved: " + std::string(resolved ? "yes" : "no"));
}

void SessionMemory::on_error_resolved(const std::string& error, const std::string& solution) {
    if (!config_.enabled) return;
    
    update_error_resolution(error, solution);
    save();
}

void SessionMemory::update_error_resolution(const std::string& pattern, const std::string& solution) {
    // Filter sensitive data before storing
    std::string filtered_pattern = filter_sensitive_data(pattern);
    std::string filtered_solution = filter_sensitive_data(solution);
    
    for (auto& err : errors_) {
        if (err.pattern == filtered_pattern || err.pattern.find(filtered_pattern) != std::string::npos) {
            err.resolved = true;
            err.solution = filtered_solution;
            err.confidence = 4;  // High confidence - resolved!
            LOG_INFO("Memory: Error resolved | Pattern: " + filtered_pattern.substr(0, 50));
            return;
        }
    }
    
    // If not found, add as new resolved error
    add_error(filtered_pattern, filtered_solution, true);
}

void SessionMemory::on_user_correction(const std::string& context, const std::string& correction) {
    if (!config_.enabled || !config_.auto_detect_preferences) return;
    
    // Try to infer preference from correction
    PreferenceEntry pref;
    pref.id = generate_id("preferences");
    pref.inferred_from = "User correction: " + context;
    pref.first_observed = std::chrono::system_clock::now();
    pref.observation_count = 1;
    
    // Try to parse what kind of preference this is
    std::string correction_lower = correction;
    std::transform(correction_lower.begin(), correction_lower.end(), correction_lower.begin(), ::tolower);
    
    if (correction_lower.find("use") != std::string::npos) {
        // Likely a preference about what to use
        pref.key = "inferred_preference";
        pref.value = correction;
    } else {
        pref.key = "user_directive";
        pref.value = correction;
    }
    
    preferences_.push_back(pref);
    save();
}

void SessionMemory::on_file_created(const std::filesystem::path& file) {
    if (!config_.enabled || !config_.auto_detect_patterns) return;
    
    // Detect patterns based on file extension
    std::string ext = file.extension().string();
    
    // If it's a header file, look for corresponding source file
    if (ext == ".h" || ext == ".hpp") {
        // Could detect "header/source pairing" pattern
    }
    
    // Save if we detected something interesting
    save();
}

// ============================================================================
// Explicit Memory Operations
// ============================================================================

void SessionMemory::remember(const std::string& category, const std::string& content,
                              int confidence, const std::vector<std::string>& tags) {
    if (!config_.enabled) return;
    
    // Filter sensitive data before storing
    std::string filtered_content = filter_sensitive_data(content);
    
    MemoryEntry entry;
    entry.id = generate_id(category);
    entry.category = category;
    entry.content = filtered_content;
    entry.confidence = confidence;
    entry.tags = tags;
    entry.created_at = std::chrono::system_clock::now();
    entry.last_accessed = std::chrono::system_clock::now();
    
    // Store in appropriate category
    if (category == "builds") {
        BuildEntry b;
        b.id = entry.id;
        b.command = filtered_content;
        b.use_count = 1;
        b.first_used = entry.created_at;
        b.last_used = entry.created_at;
        builds_.push_back(b);
    } else if (category == "errors") {
        ErrorEntry e;
        e.id = entry.id;
        e.pattern = filtered_content;
        e.confidence = confidence;
        e.first_seen = entry.created_at;
        e.last_seen = entry.created_at;
        errors_.push_back(e);
    } else if (category == "preferences") {
        PreferenceEntry p;
        p.id = entry.id;
        p.key = "explicit_memory";
        p.value = filtered_content;
        p.observation_count = 1;
        p.user_confirmed = true;
        p.first_observed = entry.created_at;
        preferences_.push_back(p);
    } else if (category == "patterns") {
        PatternEntry pat;
        pat.id = entry.id;
        pat.description = filtered_content;
        pat.confidence = confidence;
        patterns_.push_back(pat);
    }
    
    save();
}

bool SessionMemory::forget(const std::string& id) {
    // Search all categories
    auto build_it = std::find_if(builds_.begin(), builds_.end(), 
        [&id](const BuildEntry& b) { return b.id == id; });
    if (build_it != builds_.end()) {
        builds_.erase(build_it);
        save();
        return true;
    }
    
    auto err_it = std::find_if(errors_.begin(), errors_.end(), 
        [&id](const ErrorEntry& e) { return e.id == id; });
    if (err_it != errors_.end()) {
        errors_.erase(err_it);
        save();
        return true;
    }
    
    auto pit = std::find_if(preferences_.begin(), preferences_.end(), 
        [&id](const PreferenceEntry& p) { return p.id == id; });
    if (pit != preferences_.end()) {
        preferences_.erase(pit);
        save();
        return true;
    }
    
    auto patit = std::find_if(patterns_.begin(), patterns_.end(), 
        [&id](const PatternEntry& p) { return p.id == id; });
    if (patit != patterns_.end()) {
        patterns_.erase(patit);
        save();
        return true;
    }
    
    return false;
}

// ============================================================================
// Query Methods
// ============================================================================

std::vector<const BuildEntry*> SessionMemory::get_builds() const {
    std::vector<const BuildEntry*> result;
    for (const auto& b : builds_) {
        result.push_back(&b);
    }
    return result;
}

std::vector<const ErrorEntry*> SessionMemory::get_errors() const {
    std::vector<const ErrorEntry*> result;
    for (const auto& e : errors_) {
        result.push_back(&e);
    }
    return result;
}

std::vector<const ErrorEntry*> SessionMemory::get_unresolved_errors() const {
    std::vector<const ErrorEntry*> result;
    for (const auto& e : errors_) {
        if (!e.resolved) {
            result.push_back(&e);
        }
    }
    return result;
}

std::vector<const PreferenceEntry*> SessionMemory::get_preferences() const {
    std::vector<const PreferenceEntry*> result;
    for (const auto& p : preferences_) {
        result.push_back(&p);
    }
    return result;
}

std::vector<const PatternEntry*> SessionMemory::get_patterns() const {
    std::vector<const PatternEntry*> result;
    for (const auto& p : patterns_) {
        result.push_back(&p);
    }
    return result;
}

std::vector<const SessionSummary*> SessionMemory::get_recent_sessions(int count) const {
    std::vector<const SessionSummary*> result;
    
    // Get indices sorted by timestamp (descending)
    std::vector<size_t> indices(sessions_.size());
    std::iota(indices.begin(), indices.end(), 0);
    
    std::sort(indices.begin(), indices.end(), 
        [this](size_t a, size_t b) {
            return sessions_[a].timestamp > sessions_[b].timestamp;
        });
    
    int n = std::min(count, static_cast<int>(sessions_.size()));
    for (int i = 0; i < n; i++) {
        result.push_back(&sessions_[indices[i]]);
    }
    return result;
}

std::optional<const ErrorEntry*> SessionMemory::find_error(const std::string& error_pattern) const {
    for (const auto& e : errors_) {
        if (e.pattern.find(error_pattern) != std::string::npos ||
            error_pattern.find(e.pattern) != std::string::npos) {
            return &e;
        }
    }
    return std::nullopt;
}

std::optional<const BuildEntry*> SessionMemory::find_build(const std::string& command) const {
    for (const auto& b : builds_) {
        if (b.command == command || command.find(b.command) != std::string::npos) {
            return &b;
        }
    }
    return std::nullopt;
}

// ============================================================================
// Context for System Prompt
// ============================================================================

std::string SessionMemory::get_system_prompt_addition() const {
    if (!config_.enabled) return "";
    
    std::stringstream ss;
    
    // Header
    ss << "\n## Session Memory\n";
    
    // Project info
    if (!project_info_.type.empty()) {
        ss << "Project: " << project_info_.type << "\n";
    }

    // §ContextCache follow-up (2026-08-02): the volatile stats line
    // (session/build/error counters change on nearly every run) is
    // emitted at the END of the dump so unchanged memory content keeps
    // a byte-stable prefix for server-side prompt caches.
    int resolved_count = 0;
    for (const auto& e : errors_) {
        if (e.resolved) resolved_count++;
    }
    
    // Working builds — §BUG-MEMORY-BLOAT: top-N by usefulness only, with
    // commands collapsed to a capped single line. Previously EVERY
    // successful build was dumped in full; on long-lived projects (500+
    // entries incl. heredoc scripts) this alone was ~250k tokens of system
    // prompt, re-sent with every request and impossible to compress away.
    if (!builds_.empty()) {
        constexpr size_t kMaxBuildsInPrompt = 20;
        std::vector<const BuildEntry*> sorted;
        sorted.reserve(builds_.size());
        for (const auto& b : builds_)
            if (b.success) sorted.push_back(&b);
        std::sort(sorted.begin(), sorted.end(),
                  [](const BuildEntry* a, const BuildEntry* b) {
                      if (a->use_count != b->use_count)
                          return a->use_count > b->use_count;
                      return a->last_used > b->last_used;
                  });
        if (sorted.size() > kMaxBuildsInPrompt)
            sorted.resize(kMaxBuildsInPrompt);
        if (!sorted.empty()) {
            ss << "\n### Working Builds\n";
            for (const auto* b : sorted) {
                ss << "- " << truncate_memory_command(b->command)
                   << " (used " << b->use_count << " times)\n";
            }
        }
    }
    
    // Resolved errors
    bool has_resolved = false;
    for (const auto& e : errors_) {
        if (e.resolved) {
            if (!has_resolved) {
                ss << "\n### Resolved Issues\n";
                has_resolved = true;
            }
            ss << "- \"" << e.pattern << "\" → " << e.solution << " ✓\n";
        }
    }
    
    // Unresolved errors (only high confidence ones)
    bool has_unresolved = false;
    for (const auto& e : errors_) {
        if (!e.resolved && e.confidence >= 3) {
            if (!has_unresolved) {
                ss << "\n### Known Issues\n";
                has_unresolved = true;
            }
            ss << "- \"" << e.pattern << "\" (attempted: " << e.solution << ")\n";
        }
    }
    
    // Preferences
    if (!preferences_.empty()) {
        ss << "\n### Preferences\n";
        for (const auto& p : preferences_) {
            if (p.user_confirmed) {
                ss << "- " << p.value << "\n";
            }
        }
    }
    
    // Recent work
    if (!sessions_.empty()) {
        auto recent = get_recent_sessions(3);
        if (!recent.empty()) {
            ss << "\n### Recent Work\n";
            for (const auto& s : recent) {
                ss << "- " << time_point_to_iso(s->timestamp).substr(0, 10) 
                   << ": " << s->summary << "\n";
            }
        }
    }
    
    ss << "\nUse this context to avoid repeating mistakes.\n";

    // Volatile counters last — see §ContextCache note above.
    ss << "\nSessions: " << sessions_.size()
       << " | Builds: " << builds_.size()
       << " | Errors resolved: " << resolved_count << "/" << errors_.size() << "\n";

    std::string out = ss.str();
    // §BUG-MEMORY-BLOAT: final backstop — this addition rides the system
    // prompt on EVERY request, so it must never dominate the context
    // window. Per-section caps above should make this unreachable.
    constexpr size_t kMaxAdditionBytes = 16 * 1024;
    if (out.size() > kMaxAdditionBytes) {
        out = utf8_truncate_safe(out, kMaxAdditionBytes);  // §BUG-UTF8-TRUNC
        out += "\n[session memory truncated]\n";
    }
    return out;
}

// ============================================================================
// Statistics
// ============================================================================

SessionMemory::Stats SessionMemory::get_stats() const {
    Stats stats;
    stats.total_builds = static_cast<int>(builds_.size());
    stats.total_errors = static_cast<int>(errors_.size());
    stats.resolved_errors = 0;
    for (const auto& e : errors_) {
        if (e.resolved) stats.resolved_errors++;
    }
    stats.total_preferences = static_cast<int>(preferences_.size());
    stats.total_patterns = static_cast<int>(patterns_.size());
    stats.total_sessions = static_cast<int>(sessions_.size());
    stats.total_size_bytes = calculate_size();
    stats.last_updated = updated_at_;
    return stats;
}

// ============================================================================
// Memory Management
// ============================================================================

void SessionMemory::consolidate() {
    size_t initial_size = calculate_size();
    
    // 1. Prune if too many sessions
    while (static_cast<int>(sessions_.size()) > config_.max_session_history) {
        sessions_.erase(sessions_.begin());
    }
    
    // §BUG-MEMORY-BLOAT: builds_ was never pruned — bring existing
    // long-lived stores back inside the category cap on session end.
    prune_build_entries(builds_, config_.max_entries_per_category);

    // 2. Check size limit
    size_t size = calculate_size();
    size_t max_size = static_cast<size_t>(config_.max_total_size_kb) * 1024;
    
    if (size > max_size) {
        // Remove lowest confidence entries
        // Start with patterns (lowest priority)
        while (patterns_.size() > 10 && size > max_size) {
            patterns_.pop_back();
            size = calculate_size();
        }
        
        // Then old sessions
        while (sessions_.size() > 5 && size > max_size) {
            sessions_.erase(sessions_.begin());
            size = calculate_size();
        }
    }
    
    if (size < initial_size) {
        LOG_INFO("Memory: Consolidated | Size reduced from " + std::to_string(initial_size) + 
                 " to " + std::to_string(size) + " bytes");
    }
}

void SessionMemory::age_memories() {
    auto now = std::chrono::system_clock::now();
    auto age_threshold = std::chrono::hours(24 * config_.memory_age_days);
    
    // Age errors
    for (auto& e : errors_) {
        auto age = now - e.last_seen;
        if (age > age_threshold && e.resolved) {
            e.confidence = std::max(1, e.confidence - 1);
        }
    }
    
    // Remove very old resolved errors with low confidence
    errors_.erase(
        std::remove_if(errors_.begin(), errors_.end(),
            [&now, &age_threshold](const ErrorEntry& e) {
                return e.resolved && 
                       e.confidence <= 1 && 
                       (now - e.last_seen) > age_threshold * 2;
            }),
        errors_.end()
    );
    
    // Age builds
    for (auto& b : builds_) {
        auto age = now - b.last_used;
        if (age > age_threshold) {
            // Reduce confidence implicitly by reducing use_count relevance
            // (we don't delete builds, they're always useful)
        }
    }
}

void SessionMemory::clear_all_memories() {
    LOG_INFO("Memory: Clearing all memories");
    
    builds_.clear();
    errors_.clear();
    preferences_.clear();
    patterns_.clear();
    sessions_.clear();
    project_info_ = ProjectInfo();
    updated_at_ = std::chrono::system_clock::now();
    save();
    
    LOG_INFO("Memory: All memories cleared");
}

void SessionMemory::reset_for_testing() {
    builds_.clear();
    errors_.clear();
    preferences_.clear();
    patterns_.clear();
    sessions_.clear();
    project_info_ = ProjectInfo();
    config_ = Config();
    updated_at_ = std::chrono::system_clock::now();
}

// ============================================================================
// Sensitive Data Filter
// ============================================================================

std::string SessionMemory::filter_sensitive_data(const std::string& content) const {
    std::string result = content;
    
    // Pattern for API keys (long alphanumeric strings)
    // Matches: 32+ character strings of alphanumeric + common key chars
    static const std::regex api_key_pattern(R"([A-Za-z0-9+/=]{32,})");
    result = std::regex_replace(result, api_key_pattern, "[FILTERED_API_KEY]");
    
    // Pattern for Bearer tokens
    static const std::regex bearer_token_pattern(R"(Bearer\s+[A-Za-z0-9+/=_.-]{20,})", std::regex::icase);
    result = std::regex_replace(result, bearer_token_pattern, "[FILTERED_TOKEN]");
    
    // Pattern for environment variable assignments with sensitive values
    // Matches: API_KEY=..., SECRET=..., PASSWORD=..., TOKEN=...
    static const std::regex env_var_pattern(R"((API_KEY|SECRET|PASSWORD|TOKEN|PRIVATE_KEY|CREDENTIAL)\s*=\s*[^\s'",]+)", std::regex::icase);
    result = std::regex_replace(result, env_var_pattern, "$1=[FILTERED]");
    
    // Pattern for URLs with credentials
    // Matches: https://user:pass@host, http://api-key@host
    static const std::regex url_creds_pattern(R"(https?://[^:]+:[^@]+@)", std::regex::icase);
    result = std::regex_replace(result, url_creds_pattern, "[FILTERED]://");
    
    // Pattern for AWS keys
    static const std::regex aws_key_pattern(R"(AKIA[0-9A-Z]{16})");
    result = std::regex_replace(result, aws_key_pattern, "[FILTERED_AWS_KEY]");
    
    // Pattern for GitHub tokens
    static const std::regex github_token_pattern(R"(gh[pousr]_[A-Za-z0-9_]{36,})");
    result = std::regex_replace(result, github_token_pattern, "[FILTERED_GITHUB_TOKEN]");
    
    // Pattern for UUIDs (not sensitive but common noise)
    static const std::regex uuid_pattern(R"([0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12})", std::regex::icase);
    result = std::regex_replace(result, uuid_pattern, "[UUID]");
    
    return result;
}
