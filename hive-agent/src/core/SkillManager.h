#pragma once

#include <string>
#include <filesystem>
#include <vector>
#include <unordered_map>
#include <optional>
#include <memory>
#include <set>
#include <fstream>
#include <mutex>
#include <chrono>

#include "SkillsSettings.h"

// Forward declarations

/**
 * SkillMetadata - Metadata parsed from SKILL.md YAML frontmatter
 */
struct SkillMetadata {
    std::string name;           // Required: Unique skill identifier (kebab-case)
    std::string description;    // Required: Trigger condition for activation
    std::string version;        // Optional: Semantic version
    std::optional<std::string> license;        // Optional: License (MIT, Apache-2.0, etc.)
    std::optional<std::string> compatibility;  // Optional: Compatible runtimes/tools
    
    // Convert to JSON for API
    std::string to_json() const;
    
    // Convert to human-readable string
    std::string to_string() const;
};

/**
 * Skill - Complete skill with metadata, instructions, and resources
 */
struct Skill {
    SkillMetadata metadata;                            // Parsed YAML frontmatter
    std::filesystem::path path;                        // Skill directory path
    std::string instructions;                          // Full markdown body content
    
    std::vector<std::filesystem::path> scripts;        // scripts/ directory contents
    std::vector<std::filesystem::path> references;    // references/ directory contents
    std::vector<std::filesystem::path> resources;     // resources/ directory contents
    std::vector<std::filesystem::path> assets;        // assets/ directory contents
    
    bool is_valid() const { return !metadata.name.empty() && !metadata.description.empty(); }
    
    // Get formatted context for LLM
    std::string get_context() const;
    
    // Get skill directory as string
    std::string get_directory() const;
};

/**
 * SkillManager - Manages skill discovery, loading, and activation
 * 
 * Implements the Anthropic Agent Skills open standard for packaging
 * reusable capabilities and domain expertise for AI agents.
 */
class SkillManager {
public:
    // Phase 2: public default constructor. No longer a singleton.
    // Production code (Agent) constructs and owns its own instance.
    SkillManager() = default;

    // §Phase 16: per-agent SkillManager support. The skill tools live in the
    // shared ToolFactory, so they reach the manager that belongs to the
    // currently running Agent via a thread-local current pointer.
    static void set_current(SkillManager* manager);
    static SkillManager* current();

    // §Skills-Global (2026-09-30): when false, init_after_settings() skips
    // the built-in default directories (hive-brain user-global,
    // ~/.agents/skills, project-local .agents/skills) and the manager scans
    // ONLY what the host passes via skills_dirs/add_skill_directory.
    // hive-agent containers set this so the container-local home dirs
    // (wiped on container recreate — skills written there die silently)
    // leave the scan list; the read-only synced library and the shared
    // workspace global dir are passed explicitly by the host instead.
    // Must be set BEFORE init_after_settings().
    bool include_default_directories = true;

    // Initialize skill directories and settings.
    // MUST be called once before the manager is used (Agent's
    // initialize() does this after constructing settings).
    // For get_instance() default, the first call lazily invokes this.
    void init_after_settings();

    // Legacy singleton accessors (kept for tests + the daedalus-cli demo).
    // Production code should construct its own instance.
    static SkillManager* get_instance();

    // Drop the process-wide default. Tests use this to reset state.
    static void delete_default();

    ~SkillManager();
    
    // =========================================================================
    // Discovery - Finding available skills
    // =========================================================================
    
    /**
     * Discover all skills in a directory
     * @param directory Path to search for skills
     * @return Vector of discovered skills
     */
    std::vector<Skill> discover_skills(const std::filesystem::path& directory);
    
    /**
     * List skill metadata (just names and descriptions) for a directory
     * @param directory Path to search
     * @return Vector of skill metadata
     */
    std::vector<SkillMetadata> list_skill_metadata(const std::filesystem::path& directory);
    
    /**
     * Discover skills from all configured directories
     * @return Vector of all discovered skills
     */
    std::vector<Skill> discover_all_skills();
    
    // =========================================================================
    // Loading - Reading skill content
    // =========================================================================
    
    /**
     * Load a skill from its directory path
     * @param skill_path Path to the skill directory
     * @return Skill object if loaded successfully, nullopt otherwise
     */
    std::optional<Skill> load_skill(const std::filesystem::path& skill_path,
                                    std::string* skip_reason = nullptr);
    
    /**
     * Load a skill by name from any configured directory
     * @param skill_name Name of the skill
     * @return Skill object if found, nullopt otherwise
     */
    std::optional<Skill> load_skill_by_name(const std::string& skill_name);
    
    /**
     * Get skill context as a formatted string for LLM context
     * @param skill Skill to format
     * @return Formatted context string
     */
    std::string get_skill_context(const Skill& skill) const;
    
    // =========================================================================
    // Activation - Managing active skills
    // =========================================================================
    
    /**
     * Find skills relevant to a task context
     * @param task_context The current task or user message
     * @return Vector of relevant skills
     */
    std::vector<Skill> find_relevant_skills(const std::string& task_context) const;
    
    /**
     * Activate a skill (add to active skills)
     * @param skill Skill to activate
     * @return true if activated, false if already active or at max
     */
    bool activate_skill(const Skill& skill);
    
    /**
     * Deactivate a skill by name
     * @param skill_name Name of the skill to deactivate
     * @return true if deactivated, false if not found
     */
    bool deactivate_skill(const std::string& skill_name);
    
    /**
     * Deactivate all skills
     */
    void deactivate_all_skills();

    // =========================================================================
    // Persistence - Save/restore active skills across sessions
    // =========================================================================

    /**
     * @brief Save currently active skills to a JSON file
     * @param path Where to save (default: <data_root>/skills/active.json,
     *        i.e. ~/.local/share/hive-brain/skills/active.json)
     * @return true on success
     */
    bool save_active_skills(const std::filesystem::path& path = "") const;

    /**
     * @brief Restore active skills from a JSON file
     * @param path Where to load from
     * @return Number of skills restored
     */
    size_t restore_active_skills(const std::filesystem::path& path = "");
    
    /**
     * Get currently active skills
     * @return Vector of active skills
     */
    std::vector<Skill> get_active_skills() const;
    
    /**
     * Get the combined context from all active skills
     * @return Combined context string
     */
    std::string get_active_skills_context() const;
    
    /**
     * Check if a skill is active
     * @param skill_name Name of the skill
     * @return true if active
     */
    bool is_skill_active(const std::string& skill_name) const;

    // §Phase 15: per-skill gate. Host can disable skills so they never
    // appear in the catalog and can't be activated at runtime.
    // Empty = all enabled (back-compat). Names match metadata.name.
    void set_disabled_skills(const std::set<std::string>& names) { disabled_skills_ = names; }
    [[nodiscard]] const std::set<std::string>& get_disabled_skills() const { return disabled_skills_; }
    bool is_skill_disabled(const std::string& name) const { return disabled_skills_.count(name) > 0; }

    // §Phase 15: filtered view of cached skills. The full
    // get_all_cached_skills() still returns everything (for tooling
    // like the daemon admin UI), but this returns only the skills
    // visible to the agent.
    std::unordered_map<std::string, Skill> get_visible_cached_skills() const;

    // §Phase 15: filtered active-skills list.
    std::vector<Skill> get_visible_active_skills() const;
    
    // =========================================================================
    // Script Execution
    // =========================================================================
    
    /**
     * Execute a script from a skill
     * @param skill Skill containing the script
     * @param script_name Name of the script (with or without extension)
     * @param args Arguments to pass to the script
     * @return Execution result
     */
    std::string execute_script(const Skill& skill, 
                               const std::string& script_name,
                               const std::vector<std::string>& args = {});
    
    // =========================================================================
    // Configuration
    // =========================================================================
    
    /**
     * Load skill settings
     * @param settings Settings to apply
     */
    void load_settings(const SkillsSettings& settings);
    
    /**
     * Get current settings
     * @return Current skills settings
     */
    const SkillsSettings& get_settings() const { return settings_; }
    
    /**
     * Add a skill directory
     * @param directory Path to add
     */
    void add_skill_directory(const std::filesystem::path& directory);

    /**
     * Get the configured skill directories with ~ and $VARs expanded.
     * §SKILL-DIRS (2026-09-04): the agent had no way to learn where
     * skills live — discover_skills only returned paths of skills
     * that already exist, so an empty directory was invisible and the
     * model could not know where to CREATE one. This exposes the scan
     * list so DiscoverSkillsTool can report it.
     * @return Absolute paths scanned for skills, in discovery order
     *         (hive-brain user-global, ~/.agents/skills, .agents/skills,
     *         then any host-configured dirs).
     */
    std::vector<std::filesystem::path> skill_directories() const {
        return expand_skill_directories();
    }
    
    // =========================================================================
    // Utility
    // =========================================================================
    
    /**
     * Refresh skill cache (re-discover all skills). Live rescan: drops
     * vanished skills, picks up skills created since the last scan.
     */
    void refresh_cache();

    /**
     * §Skills-Global: refresh the cache only when the last scan is older
     * than max_age — the lazy-loading hook. Called per turn by the LLM
     * client so a skill dropped into a scanned directory at runtime
     * appears without an agent restart.
     * @return true if a rescan actually happened
     */
    bool maybe_refresh_cache(std::chrono::seconds max_age);

    /**
     * §Skills-Global: one record per subdirectory that looked like a
     * skill but was dropped during the last discovery scan (no SKILL.md,
     * bad frontmatter, missing name/description...). Surfaced by the
     * discover_skills tool so a dropped skill is never silent.
     */
    struct SkippedSkill {
        std::filesystem::path path;
        std::string reason;
    };
    std::vector<SkippedSkill> get_last_scan_skipped() const;
    
    /**
     * Validate a skill directory
     * @param skill_path Path to validate
     * @return true if valid skill
     */
    bool validate_skill(const std::filesystem::path& skill_path) const;
    
    /**
     * Get all cached skills (from all directories)
     * @return Map of skill name to skill
     */
    const std::unordered_map<std::string, Skill>& get_all_cached_skills() const { return cached_skills_; }
    
    /**
     * Search skills by query (matches name and description)
     * @param query Search query
     * @return Matching skills
     */
    std::vector<Skill> search_skills(const std::string& query) const;

private:
    // Singleton
    // (instance_ removed in Phase 2 — no static singleton state)
    
    // Settings
    SkillsSettings settings_;
    
    // Cached skills (discovered skills indexed by name)
    std::unordered_map<std::string, Skill> cached_skills_;
    // §fix (2026-07-18): guards cached_skills_ writes — discover_all_skills()
    // runs discover_skills() on ThreadPool workers, which would otherwise
    // perform concurrent unordered_map writes (UB).
    mutable std::mutex cache_mutex_;   // mutable: get_last_scan_skipped() is const
    std::vector<SkippedSkill> last_scan_skipped_;  // guarded by cache_mutex_
    std::chrono::steady_clock::time_point last_scan_{};  // guarded by cache_mutex_
    std::set<std::string>                  disabled_skills_;  // §Phase 15
    
    // Active skills (currently loaded in context)
    std::set<std::string> active_skill_names_;

    // Parse SKILL.md content
    std::optional<SkillMetadata> parse_skill_metadata(const std::string& content) const;
    
    // Parse YAML frontmatter from SKILL.md
    std::optional<std::pair<std::string, std::string>> parse_frontmatter(const std::string& content) const;
    
    // Load skill resources (scripts, references, etc.)
    void load_skill_resources(Skill& skill) const;
    
    // Expand skill directory paths (handles ~ and environment variables)
    std::vector<std::filesystem::path> expand_skill_directories() const;
    
    // Re-apply ordered discovery results to the cache so later
    // directories deterministically shadow earlier ones (§Skills-Global).
    void apply_ordered_priority(const std::vector<Skill>& ordered);

    // Check if a skill matches a task context (simple keyword matching)
    bool matches_context(const Skill& skill, const std::string& context) const;
    
    // Find a script in skill's scripts directory
    std::optional<std::filesystem::path> find_script(const Skill& skill, 
                                                     const std::string& script_name) const;
};

// =========================================================================
// Inlined implementations for frequently used methods
// =========================================================================

inline bool SkillManager::is_skill_active(const std::string& skill_name) const {
    // §Phase 15: a disabled skill never reports active, even if it is
    // still present in active_skill_names_ (the disable is runtime, the
    // internal list keeps it). Callers use this as the runtime gate
    // (LLMClient activate flow, SkillTool is_active reporting).
    if (disabled_skills_.count(skill_name) > 0) return false;
    return active_skill_names_.find(skill_name) != active_skill_names_.end();
}

inline std::vector<Skill> SkillManager::get_active_skills() const {
    std::vector<Skill> active;
    for (const auto& name : active_skill_names_) {
        auto it = cached_skills_.find(name);
        if (it != cached_skills_.end()) {
            active.push_back(it->second);
        }
    }
    return active;
}
