#pragma once

#include <string>
#include <vector>

/**
 * SkillsSettings - Configuration for the skills system
 * 
 * Defines how hive-brain discovers and uses Agent Skills.
 */
struct SkillsSettings {
    bool enabled = true;                           // Enable/disable skills system
    std::vector<std::string> directories;         // Skill search directories
    int max_active_skills = 5;                    // Max skills to load in context
    bool auto_discover = true;                    // Auto-discover skills on startup
};
