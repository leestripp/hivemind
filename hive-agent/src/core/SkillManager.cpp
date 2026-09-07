#include "SkillManager.h"
#include "core/ThreadPool.h"
#include "library/paths.h"
#include "core/settings.h"
#include <nlohmann/json.hpp>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <cctype>
#include <regex>
#include <iostream>
#include <cerrno>
#include <sys/stat.h>

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
// Static member initialization
//
// Process-wide default instance for legacy callers (tests + the
// daedalus-cli demo). Production code (Agent) constructs its own
// SkillManager.
namespace {
    SkillManager*& default_skill_manager_ref() {
        static SkillManager* g_default = nullptr;
        return g_default;
    }

    SkillManager*& current_skill_manager_ref() {
        thread_local SkillManager* g_current = nullptr;
        return g_current;
    }
}

void SkillManager::set_current(SkillManager* manager) {
    current_skill_manager_ref() = manager;
}

SkillManager* SkillManager::current() {
    SkillManager* p = current_skill_manager_ref();
    if (p) return p;
    return get_instance();
}

SkillManager* SkillManager::get_instance() {
    auto& p = default_skill_manager_ref();
    if (!p) {
        p = new SkillManager();
        p->init_after_settings();
    }
    return p;
}

void SkillManager::delete_default() {
    auto& p = default_skill_manager_ref();
    delete p;
    p = nullptr;
}

// SkillManager constructor body moved to `init_after_settings()` to allow
// construction before the settings system exists. The header declares the
// ctor as `= default`; init_after_settings() must be called once before
// the manager is used (Agent's initialize() does this).
//
// For the legacy daedalus-cli demo + tests, we keep an init-on-first-use
// path: get_instance() returns a default-constructed instance, and the
// first call to a method that needs settings lazily calls
// init_after_settings() if it hasn't been called yet.

void SkillManager::init_after_settings() {
    // §v0.5.0: Try to load skill settings from SettingsStore if available.
    // If SettingsStore isn't available or has no skill settings, use defaults.
    try {
        const hive::SettingsStore& store = hive::SettingsStore::instance();
        // v0.5.0: SkillsSettings are not yet stored in SettingsStore.
        // For now, just use the directory defaults. Future: load from
        // SettingsStore's kv_settings.
        (void)store;  // suppress unused warning until skill settings are stored
    } catch (...) {
        // SettingsStore not available yet, use defaults
    }

    // Add default skill directories.
    // Discovery order:
    //   1. <data_root>/skills/               - daedalus-specific user-global skills
    //      (XDG_DATA_HOME/hive-brain/skills, default
    //      ~/.local/share/hive-brain/skills)
    //   2. ~/.agents/skills/                - Anthropic Agent Skills standard (user-global)
    //   3. .agents/skills/                  - Anthropic Agent Skills standard (project-local)
    const char* home = std::getenv("HOME");
    settings_.directories.push_back(
        hive::Paths::global().global_skills_dir().string());
    if (home) {
        settings_.directories.push_back(std::string(home) + "/.agents/skills/");
    }
    settings_.directories.push_back(".agents/skills/");
}

SkillManager::~SkillManager() {
    // Clean up if needed
}

// =========================================================================
// SkillMetadata methods
// =========================================================================

std::string SkillMetadata::to_json() const {
    std::ostringstream oss;
    oss << "{";
    oss << "\"name\": \"" << name << "\", ";
    oss << "\"description\": \"" << description << "\"";
    if (!version.empty()) {
        oss << ", \"version\": \"" << version << "\"";
    }
    if (license.has_value()) {
        oss << ", \"license\": \"" << *license << "\"";
    }
    if (compatibility.has_value()) {
        oss << ", \"compatibility\": \"" << *compatibility << "\"";
    }
    oss << "}";
    return oss.str();
}

std::string SkillMetadata::to_string() const {
    std::ostringstream oss;
    oss << "=== " << name << " ===" << '\n';
    oss << "Description: " << description << '\n';
    if (!version.empty()) {
        oss << "Version: " << version << '\n';
    }
    if (license.has_value()) {
        oss << "License: " << *license << '\n';
    }
    if (compatibility.has_value()) {
        oss << "Compatibility: " << *compatibility << '\n';
    }
    return oss.str();
}

// =========================================================================
// Skill methods
// =========================================================================

std::string Skill::get_context() const {
    std::ostringstream oss;
    oss << "## Skill: " << metadata.name << '\n' << '\n';
    oss << instructions << '\n';
    
    if (!scripts.empty()) {
        oss << '\n' << "### Available Scripts:" << '\n';
        for (const auto& script : scripts) {
            oss << "- " << script.filename().string() << '\n';
        }
    }
    
    return oss.str();
}

std::string Skill::get_directory() const {
    return path.string();
}

// =========================================================================
// Discovery - Finding available skills
// =========================================================================

std::vector<Skill> SkillManager::discover_skills(const std::filesystem::path& directory) {
    std::vector<Skill> skills;
    
    if (!std::filesystem::exists(directory)) {
        return skills;
    }
    
    if (!std::filesystem::is_directory(directory)) {
        return skills;
    }
    
    try {
        for (const auto& entry : std::filesystem::directory_iterator(directory)) {
            if (!entry.is_directory()) {
                continue;
            }
            
            auto skill_path = entry.path();
            auto skill = load_skill(skill_path);
            if (skill.has_value() && skill->is_valid()) {
                skills.push_back(*skill);
                // Also cache by name
                // §fix (2026-07-18): locked — discover_all_skills() calls
                // this on ThreadPool workers concurrently.
                std::lock_guard<std::mutex> lk(cache_mutex_);
                cached_skills_[skill->metadata.name] = *skill;
            }
        }
    } catch (const std::exception& e) {
        LOG_CERR(INFO, "Error discovering skills in " << directory << ": " << e.what());
    }
    
    return skills;
}

std::vector<SkillMetadata> SkillManager::list_skill_metadata(const std::filesystem::path& directory) {
    std::vector<SkillMetadata> metadata;
    
    auto skills = discover_skills(directory);
    for (const auto& skill : skills) {
        metadata.push_back(skill.metadata);
    }
    
    return metadata;
}

std::vector<Skill> SkillManager::discover_all_skills() {
    auto directories = expand_skill_directories();

    if (directories.size() <= 1) {
        // Single directory - no parallelism needed
        std::vector<Skill> all_skills;
        for (const auto& dir : directories) {
            auto skills = discover_skills(dir);
            all_skills.insert(all_skills.end(), skills.begin(), skills.end());
        }
        return all_skills;
    }

    // Multiple directories - parallelize via ThreadPool
    using hive::concurrent::ThreadPool;
    using hive::concurrent::TaskPriority;

    std::vector<std::future<std::vector<Skill>>> futures;
    for (const auto& dir : directories) {
        futures.push_back(
            ThreadPool::get_instance().submit(
                TaskPriority::NORMAL,
                [this, dir]() { return this->discover_skills(dir); }
            )
        );
    }

    // Collect results in order
    std::vector<Skill> all_skills;
    for (auto& fut : futures) {
        auto skills = fut.get();
        all_skills.insert(all_skills.end(), skills.begin(), skills.end());
    }
    return all_skills;
}

// =========================================================================
// Loading - Reading skill content
// =========================================================================

std::optional<Skill> SkillManager::load_skill(const std::filesystem::path& skill_path) {
    if (!validate_skill(skill_path)) {
        return std::nullopt;
    }
    
    Skill skill;
    skill.path = skill_path;
    
    // Read SKILL.md
    auto skill_md_path = skill_path / "SKILL.md";
    if (!std::filesystem::exists(skill_md_path)) {
        return std::nullopt;
    }
    
    try {
        std::ifstream file(skill_md_path);
        if (!file.is_open()) {
            return std::nullopt;
        }
        
        std::stringstream buffer;
        buffer << file.rdbuf();
        std::string content = buffer.str();
        file.close();
        
        // Parse frontmatter and content
        auto parsed = parse_frontmatter(content);
        if (!parsed.has_value()) {
            return std::nullopt;
        }
        
        auto frontmatter = parsed->first;
        skill.instructions = parsed->second;
        
        // Parse YAML frontmatter
        auto metadata = parse_skill_metadata(frontmatter);
        if (!metadata.has_value()) {
            return std::nullopt;
        }
        skill.metadata = *metadata;
        
        // Load resources (scripts, references, etc.)
        load_skill_resources(skill);
        
        return skill;
        
    } catch (const std::exception& e) {
        LOG_CERR(INFO, "Error loading skill from " << skill_path << ": " << e.what());
        return std::nullopt;
    }
}

std::optional<Skill> SkillManager::load_skill_by_name(const std::string& skill_name) {
    // First check cache
    auto it = cached_skills_.find(skill_name);
    if (it != cached_skills_.end()) {
        return it->second;
    }
    
    // Search in all directories
    auto directories = expand_skill_directories();
    for (const auto& dir : directories) {
        if (!std::filesystem::exists(dir)) {
            continue;
        }
        
        auto skill_path = dir / skill_name;
        if (std::filesystem::exists(skill_path) && std::filesystem::is_directory(skill_path)) {
            auto skill = load_skill(skill_path);
            if (skill.has_value()) {
                cached_skills_[skill_name] = *skill;
                return skill;
            }
        }
    }
    
    return std::nullopt;
}

std::string SkillManager::get_skill_context(const Skill& skill) const {
    return skill.get_context();
}

// =========================================================================
// Activation - Managing active skills
// =========================================================================

std::vector<Skill> SkillManager::find_relevant_skills(const std::string& task_context) const {
    std::vector<Skill> relevant;
    
    // Convert to lowercase for comparison
    std::string lower_context = task_context;
    std::transform(lower_context.begin(), lower_context.end(), lower_context.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    
    for (const auto& [name, skill] : cached_skills_) {
        if (matches_context(skill, lower_context)) {
            relevant.push_back(skill);
        }
    }
    
    // Limit to max_active_skills
    if (static_cast<int>(relevant.size()) > settings_.max_active_skills) {
        relevant.resize(settings_.max_active_skills);
    }
    
    return relevant;
}

bool SkillManager::activate_skill(const Skill& skill) {
    // §Phase 15: host-disabled skills never activate.
    if (is_skill_disabled(skill.metadata.name)) return false;
    if (!settings_.enabled) {
        return false;
    }

    // Check if at max
    if (static_cast<int>(active_skill_names_.size()) >= settings_.max_active_skills) {
        // Replace oldest or least relevant skill
        // For now, just don't add
        return false;
    }

    active_skill_names_.insert(skill.metadata.name);
    return true;
}

bool SkillManager::deactivate_skill(const std::string& skill_name) {
    auto it = active_skill_names_.find(skill_name);
    if (it != active_skill_names_.end()) {
        active_skill_names_.erase(it);
        return true;
    }
    return false;
}

void SkillManager::deactivate_all_skills() {
    active_skill_names_.clear();
}

// =========================================================================
// Persistence
// =========================================================================

static std::filesystem::path get_default_active_skills_path() {
    // Stored at: <data_root>/skills/active.json
    return hive::Paths::global().user_data_root() / "skills" / "active.json";
}

bool SkillManager::save_active_skills(const std::filesystem::path& path) const {
    std::filesystem::path target = path.empty() ? get_default_active_skills_path() : path;
    try {
        // Ensure parent dir exists
        std::filesystem::create_directories(target.parent_path());

        nlohmann::json j;
        j["active_skills"] = nlohmann::json::array();
        for (const auto& name : active_skill_names_) {
            j["active_skills"].push_back(name);
        }
        j["saved_at"] = std::chrono::system_clock::now().time_since_epoch().count();

        std::ofstream f(target);
        if (!f) return false;
        f << j.dump(2);
        return f.good();
    } catch (const std::exception& e) {
        LOG_CERR(ERROR, "[SkillManager] Failed to save active skills: " << e.what());
        return false;
    }
}

size_t SkillManager::restore_active_skills(const std::filesystem::path& path) {
    std::filesystem::path source = path.empty() ? get_default_active_skills_path() : path;
    if (!std::filesystem::exists(source)) {
        return 0;
    }

    try {
        std::ifstream f(source);
        if (!f) return 0;

        nlohmann::json j;
        f >> j;
        if (!j.contains("active_skills") || !j["active_skills"].is_array()) {
            return 0;
        }

        size_t restored = 0;
        for (const auto& name_json : j["active_skills"]) {
            if (!name_json.is_string()) continue;
            std::string name = name_json.get<std::string>();
            // Try to load the skill
            auto skill = load_skill_by_name(name);
            if (skill.has_value()) {
                activate_skill(*skill);
                restored++;
            }
        }
        return restored;
    } catch (const std::exception& e) {
        LOG_CERR(ERROR, "[SkillManager] Failed to restore active skills: " << e.what());
        return 0;
    }
}

std::string SkillManager::get_active_skills_context() const {
    std::ostringstream oss;
    
    int count = 0;
    for (const auto& name : active_skill_names_) {
        auto it = cached_skills_.find(name);
        if (it != cached_skills_.end()) {
            if (count > 0) {
                oss << '\n' << '\n';
            }
            oss << it->second.get_context();
            count++;
        }
    }
    
    return oss.str();
}

// =========================================================================
// Script Execution
// =========================================================================

std::string SkillManager::execute_script(const Skill& skill,
                                         const std::string& script_name,
                                         const std::vector<std::string>& args) {
    auto script_path = find_script(skill, script_name);
    if (!script_path.has_value()) {
        return "Script not found: " + script_name;
    }
    
    // Security: Ensure script is within skill's scripts directory
    auto scripts_dir = skill.path / "scripts";
    if (!script_path->string().starts_with(scripts_dir.string())) {
        return "Error: Script path traversal detected";
    }
    
    // Build command
    std::ostringstream cmd;
    cmd << script_path->string();
    for (const auto& arg : args) {
        cmd << " \"" << arg << "\"";
    }
    
    // Execute (this would typically use execute_bash tool)
    // For now, return the command that would be executed
    return "Would execute: " + cmd.str();
}

// =========================================================================
// Configuration
// =========================================================================

void SkillManager::load_settings(const SkillsSettings& settings) {
    settings_ = settings;
    
    // Refresh cache with new settings
    if (settings_.auto_discover) {
        refresh_cache();
    }
}

void SkillManager::add_skill_directory(const std::filesystem::path& directory) {
    settings_.directories.push_back(directory.string());
}

// =========================================================================
// Utility
// =========================================================================

void SkillManager::refresh_cache() {
    cached_skills_.clear();
    discover_all_skills();
}

bool SkillManager::validate_skill(const std::filesystem::path& skill_path) const {
    if (!std::filesystem::exists(skill_path)) {
        return false;
    }
    
    if (!std::filesystem::is_directory(skill_path)) {
        return false;
    }
    
    // Must have SKILL.md
    auto skill_md = skill_path / "SKILL.md";
    return std::filesystem::exists(skill_md);
}

std::vector<Skill> SkillManager::search_skills(const std::string& query) const {
    std::vector<Skill> results;
    
    std::string lower_query = query;
    std::transform(lower_query.begin(), lower_query.end(), lower_query.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    
    for (const auto& [name, skill] : cached_skills_) {
        // Search in name
        std::string lower_name = name;
        std::transform(lower_name.begin(), lower_name.end(), lower_name.begin(),
                      [](unsigned char c) { return std::tolower(c); });
        
        if (lower_name.find(lower_query) != std::string::npos) {
            results.push_back(skill);
            continue;
        }
        
        // Search in description
        std::string lower_desc = skill.metadata.description;
        std::transform(lower_desc.begin(), lower_desc.end(), lower_desc.begin(),
                      [](unsigned char c) { return std::tolower(c); });
        
        if (lower_desc.find(lower_query) != std::string::npos) {
            results.push_back(skill);
        }
    }
    
    return results;
}

// =========================================================================
// Private methods
// =========================================================================

std::optional<std::pair<std::string, std::string>> SkillManager::parse_frontmatter(const std::string& content) const {
    // Manual frontmatter parser. Previously this used std::regex with the
    // pattern ^---\s*\n([\s\S]*?)\n---\s*\n?([\s\S]*)$, but libstdc++'s
    // std::regex implementation segfaults on large SKILL.md files (>~25 KB)
    // that contain multiple '---' horizontal rules. The C++ standard
    // explicitly allows std::regex to fail on complex patterns; libstdc++
    // does so via stack exhaustion in the NFA matcher.
    //
    // Manual approach: find the first line, verify it's "---", then find the
    // next line that is exactly "---" (or starts with "---"). Frontmatter is
    // everything between; body is everything after.
    if (content.size() < 7) {  // minimum: "---\n---\n" = 8 chars
        return std::nullopt;
    }

    // Check first line is "---" (with optional trailing whitespace)
    size_t pos = 0;
    // Skip leading whitespace (don't be too strict — markdown allows it)
    while (pos < content.size() && (content[pos] == ' ' || content[pos] == '\t')) {
        ++pos;
    }
    if (content.compare(pos, 3, "---") != 0) {
        return std::nullopt;
    }
    pos += 3;
    // Skip to end of first line
    while (pos < content.size() && content[pos] != '\n') {
        ++pos;
    }
    if (pos >= content.size()) {
        return std::nullopt;
    }
    ++pos;  // past the newline

    // Find the closing "---" on its own line
    size_t fm_start = pos;
    size_t fm_end = std::string::npos;
    while (pos < content.size()) {
        // Check if this line is "---" (or starts with "---" followed by whitespace/EOL)
        size_t line_start = pos;
        // Skip leading whitespace
        while (pos < content.size() && (content[pos] == ' ' || content[pos] == '\t')) {
            ++pos;
        }
        if (content.compare(pos, 3, "---") == 0) {
            // Verify it's a standalone "---" line (followed by whitespace or newline or EOF)
            size_t after = pos + 3;
            if (after >= content.size() ||
                content[after] == '\n' || content[after] == '\r' ||
                content[after] == ' '  || content[after] == '\t') {
                fm_end = line_start;
                pos = after;
                break;
            }
        }
        // Skip to next line
        while (pos < content.size() && content[pos] != '\n') {
            ++pos;
        }
        if (pos < content.size()) ++pos;
    }

    if (fm_end == std::string::npos) {
        return std::nullopt;  // No closing "---" found
    }

    // Skip trailing whitespace/newlines after closing "---"
    while (pos < content.size() && (content[pos] == '\n' || content[pos] == '\r' ||
                                    content[pos] == ' '  || content[pos] == '\t')) {
        ++pos;
    }

    std::string frontmatter = content.substr(fm_start, fm_end - fm_start);
    std::string body = content.substr(pos);
    return std::make_pair(frontmatter, body);
}

std::optional<SkillMetadata> SkillManager::parse_skill_metadata(const std::string& frontmatter) const {
    SkillMetadata metadata;
    
    std::istringstream iss(frontmatter);
    std::string line;
    
    while (std::getline(iss, line)) {
        // Trim whitespace
        line.erase(0, line.find_first_not_of(" \t\r\n"));
        line.erase(line.find_last_not_of(" \t\r\n") + 1);
        
        if (line.empty() || line[0] == '#') {
            continue;  // Skip empty lines and comments
        }
        
        // Find colon separator
        auto colon_pos = line.find(':');
        if (colon_pos == std::string::npos) {
            continue;
        }
        
        std::string key = line.substr(0, colon_pos);
        std::string value = line.substr(colon_pos + 1);
        
        // Trim whitespace from key and value
        key.erase(0, key.find_first_not_of(" \t"));
        key.erase(key.find_last_not_of(" \t") + 1);
        value.erase(0, value.find_first_not_of(" \t"));
        
        // Remove surrounding quotes if present
        if (value.size() >= 2) {
            if ((value.front() == '"' && value.back() == '"') ||
                (value.front() == '\'' && value.back() == '\'')) {
                value = value.substr(1, value.size() - 2);
            }
        }
        
        if (key == "name") {
            metadata.name = value;
        } else if (key == "description") {
            metadata.description = value;
        } else if (key == "version") {
            metadata.version = value;
        } else if (key == "license") {
            metadata.license = value;
        } else if (key == "compatibility") {
            metadata.compatibility = value;
        }
    }
    
    if (metadata.name.empty() || metadata.description.empty()) {
        return std::nullopt;
    }
    
    return metadata;
}

void SkillManager::load_skill_resources(Skill& skill) const {
    // Load scripts/
    auto scripts_dir = skill.path / "scripts";
    if (std::filesystem::exists(scripts_dir) && std::filesystem::is_directory(scripts_dir)) {
        try {
            for (const auto& entry : std::filesystem::directory_iterator(scripts_dir)) {
                if (entry.is_regular_file()) {
                    skill.scripts.push_back(entry.path());
                }
            }
        } catch (const std::exception&) {}
    }
    
    // Load references/
    auto references_dir = skill.path / "references";
    if (std::filesystem::exists(references_dir) && std::filesystem::is_directory(references_dir)) {
        try {
            for (const auto& entry : std::filesystem::directory_iterator(references_dir)) {
                if (entry.is_regular_file()) {
                    skill.references.push_back(entry.path());
                }
            }
        } catch (const std::exception&) {}
    }
    
    // Load resources/
    auto resources_dir = skill.path / "resources";
    if (std::filesystem::exists(resources_dir) && std::filesystem::is_directory(resources_dir)) {
        try {
            for (const auto& entry : std::filesystem::directory_iterator(resources_dir)) {
                if (entry.is_regular_file()) {
                    skill.resources.push_back(entry.path());
                }
            }
        } catch (const std::exception&) {}
    }
    
    // Load assets/
    auto assets_dir = skill.path / "assets";
    if (std::filesystem::exists(assets_dir) && std::filesystem::is_directory(assets_dir)) {
        try {
            for (const auto& entry : std::filesystem::directory_iterator(assets_dir)) {
                if (entry.is_regular_file()) {
                    skill.assets.push_back(entry.path());
                }
            }
        } catch (const std::exception&) {}
    }
}

std::vector<std::filesystem::path> SkillManager::expand_skill_directories() const {
    std::vector<std::filesystem::path> expanded;
    
    for (const auto& dir : settings_.directories) {
        std::string expanded_dir = dir;
        
        // Expand ~
        if (!expanded_dir.empty() && expanded_dir[0] == '~') {
            const char* home = std::getenv("HOME");
            if (home) {
                expanded_dir = std::string(home) + expanded_dir.substr(1);
            }
        }
        
        // Expand environment variables
        // Simple implementation - could be enhanced
        size_t pos = 0;
        while ((pos = expanded_dir.find('$', pos)) != std::string::npos) {
            if (pos + 1 < expanded_dir.size()) {
                char var_name[256] = {0};
                size_t var_end = pos + 1;
                size_t var_idx = 0;
                
                // Get variable name
                while (var_end < expanded_dir.size() && var_idx < 255) {
                    char c = expanded_dir[var_end];
                    if (std::isalnum(c) || c == '_') {
                        var_name[var_idx++] = c;
                        var_end++;
                    } else {
                        break;
                    }
                }
                
                const char* var_value = std::getenv(var_name);
                if (var_value) {
                    expanded_dir = expanded_dir.substr(0, pos) + var_value + expanded_dir.substr(var_end);
                }
                pos++;
            }
        }

        expanded.push_back(expanded_dir);
    }

    // Dedup by canonical path to avoid processing the same directory twice.
    std::vector<std::filesystem::path> deduped;
    for (auto& dir : expanded) {
        std::error_code ec;
        auto canon = std::filesystem::weakly_canonical(dir, ec);
        const std::string key = ec ? dir.string() : canon.string();
        bool already = false;
        for (const auto& d : deduped) {
            std::error_code ec2;
            auto dcanon = std::filesystem::weakly_canonical(d, ec2);
            const std::string dkey = ec2 ? d.string() : dcanon.string();
            if (dkey == key) { already = true; break; }
        }
        if (!already) deduped.push_back(dir);
    }

    return deduped;
}

bool SkillManager::matches_context(const Skill& skill, const std::string& context) const {
    // Simple keyword matching between context and skill metadata
    // This is a basic implementation - could use more sophisticated matching
    
    std::string name_lower = skill.metadata.name;
    std::transform(name_lower.begin(), name_lower.end(), name_lower.begin(),
                 [](unsigned char c) { return std::tolower(c); });
    
    std::string desc_lower = skill.metadata.description;
    std::transform(desc_lower.begin(), desc_lower.end(), desc_lower.begin(),
                 [](unsigned char c) { return std::tolower(c); });
    
    // Check for keyword matches
    // Split context into words
    std::istringstream iss(context);
    std::string word;
    int match_count = 0;
    int total_words = 0;
    
    while (iss >> word) {
        // Skip very short words
        if (word.length() < 3) continue;
        
        total_words++;
        
        // Check if word appears in name or description
        if (name_lower.find(word) != std::string::npos ||
            desc_lower.find(word) != std::string::npos) {
            match_count++;
        }
    }
    
    // Consider relevant if at least 20% of words match
    // or if any significant word matches
    if (total_words > 0) {
        return (match_count * 5 >= total_words) || match_count >= 2;
    }
    
    // Fallback: check for specific keywords
    const std::vector<std::string> keywords = {
        skill.metadata.name,
        skill.metadata.description
    };
    
    int keywords_found = 0;
    for (const auto& kw : keywords) {
        if (context.find(kw) != std::string::npos) {
            keywords_found++;
        }
    }
    
    return keywords_found > 0;
}

std::optional<std::filesystem::path> SkillManager::find_script(const Skill& skill,
                                                               const std::string& script_name) const {
    // First try exact match
    for (const auto& script : skill.scripts) {
        if (script.filename().string() == script_name) {
            return script;
        }
    }
    
    // Try without extension
    for (const auto& script : skill.scripts) {
        std::string filename = script.filename().string();
        size_t dot_pos = filename.rfind('.');
        if (dot_pos != std::string::npos) {
            std::string basename = filename.substr(0, dot_pos);
            if (basename == script_name) {
                return script;
            }
        }
    }
    
    // Try partial match
    for (const auto& script : skill.scripts) {
        std::string filename = script.filename().string();
        if (filename.find(script_name) != std::string::npos) {
            return script;
        }
    }
    
    return std::nullopt;
}

// =========================================================================
// §Phase 15: filtered views. get_all_cached_skills() and get_active_skills()
// stay unfiltered (used by daemon UI / debug tools). The agent only ever
// sees these filtered versions.
// =========================================================================

std::unordered_map<std::string, Skill> SkillManager::get_visible_cached_skills() const {
    std::unordered_map<std::string, Skill> out;
    out.reserve(cached_skills_.size());
    for (const auto& [name, skill] : cached_skills_) {
        if (is_skill_disabled(name)) continue;
        out.emplace(name, skill);
    }
    return out;
}

std::vector<Skill> SkillManager::get_visible_active_skills() const {
    std::vector<Skill> active;
    for (const auto& name : active_skill_names_) {
        if (is_skill_disabled(name)) continue;
        auto it = cached_skills_.find(name);
        if (it != cached_skills_.end()) {
            active.push_back(it->second);
        }
    }
    return active;
}
