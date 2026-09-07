#include "SkillTool.h"
#include "BaseTool.h"
#include "../core/SkillManager.h"
#include "../core/FileIO.h"

#include <filesystem>
#include <iostream>

using json = nlohmann::json;

// ============================================================================
// DiscoverSkillsTool
// ============================================================================

DiscoverSkillsTool::DiscoverSkillsTool() {
    name_ = "discover_skills";
    description_ = "List available skills, their metadata, and the skill directories. Use this to find relevant skills or to learn where to create a new one.";
}

json DiscoverSkillsTool::get_definition() const {
    return {
        {"type", "function"},
        {"function", {
            {"name", get_name()},
            {"description", "Lists all available skills with their metadata (name, description, version, path), plus skill_directories: the directories scanned for skills (hive-brain user-global, ~/.agents/skills, project-local .agents/skills, and any host-configured dirs). To CREATE a new skill, write a directory named after it containing a SKILL.md file into one of those directories (prefer the project-local .agents/skills/ for project-specific skills, ~/.agents/skills/ for reusable ones), then call discover_skills again to pick it up."},
            {"parameters", {
                {"type", "object"},
                {"properties", {
                    {"directory", {
                        {"type", "string"},
                        {"description", "Optional: Directory to search for skills. Defaults to configured skill directories."}
                    }}
                }},
                {"required", json::array()}
            }}
        }}
    };
}

json DiscoverSkillsTool::execute(const json& /*args*/, FileIO& /*file_io*/) {
    SkillManager* skill_manager = SkillManager::current();
    if (!skill_manager) {
        return json::object({
            {"status", "error"},
            {"message", "SkillManager not initialized"}
        });
    }
    
    json result = json::object();
    result["status"] = "success";
    result["skills"] = json::array();
    
    const auto& skills = skill_manager->get_all_cached_skills();
    for (const auto& [name, skill] : skills) {
        json skill_info = json::object();
        skill_info["name"] = skill.metadata.name;
        skill_info["description"] = skill.metadata.description;
        skill_info["version"] = skill.metadata.version;
        skill_info["path"] = skill.path.string();
        skill_info["is_active"] = skill_manager->is_skill_active(skill.metadata.name);
        
        if (skill.metadata.license.has_value()) {
            skill_info["license"] = *skill.metadata.license;
        }
        
        result["skills"].push_back(skill_info);
    }
    
    result["count"] = skills.size();

    // §SKILL-DIRS (2026-09-04): tell the agent WHERE skills live.
    // Previously only existing skills' paths were returned, so an empty
    // directory was invisible and the model had no idea where to create
    // a new skill. List every configured directory (expanded, absolute)
    // with an exists flag so the agent can pick a target.
    result["skill_directories"] = json::array();
    for (const auto& dir : skill_manager->skill_directories()) {
        const auto abs = std::filesystem::absolute(dir);
        result["skill_directories"].push_back(json::object({
            {"path", abs.string()},
            {"exists", std::filesystem::exists(abs)}
        }));
    }

    return result;
}

// ============================================================================
// LoadSkillTool
// ============================================================================

LoadSkillTool::LoadSkillTool() {
    name_ = "load_skill";
    description_ = "Load a skill by name so its instructions are included in the agent context.";
}

json LoadSkillTool::get_definition() const {
    return {
        {"type", "function"},
        {"function", {
            {"name", get_name()},
            {"description", "Loads a specific skill by name, adding its instructions to the agent's context. Use this when you need specialized knowledge or workflows."},
            {"parameters", {
                {"type", "object"},
                {"properties", {
                    {"skill_name", {
                        {"type", "string"},
                        {"description", "Name of the skill to load (e.g., 'cpp-best-practices', 'cmake-helper', 'git-workflow', 'code-review')"}
                    }}
                }},
                {"required", {"skill_name"}}
            }}
        }}
    };
}

json LoadSkillTool::execute(const json& args, FileIO& /*file_io*/) {
    SkillManager* skill_manager = SkillManager::current();
    if (!skill_manager) {
        return json::object({
            {"status", "error"},
            {"message", "SkillManager not initialized"}
        });
    }
    
    std::string skill_name;
    if (args.contains("skill_name")) {
        skill_name = args["skill_name"].get<std::string>();
    } else {
        return json::object({
            {"status", "error"},
            {"message", "Missing required parameter: skill_name"}
        });
    }
    
    auto skill = skill_manager->load_skill_by_name(skill_name);
    if (!skill.has_value()) {
        return json::object({
            {"status", "error"},
            {"message", "Skill not found: " + skill_name},
            {"hint", "Use discover_skills to see available skills"}
        });
    }
    
    bool activated = skill_manager->activate_skill(*skill);
    
    json result = json::object();
    result["status"] = "success";
    result["skill_name"] = skill->metadata.name;
    result["description"] = skill->metadata.description;
    result["activated"] = activated;
    result["context"] = skill_manager->get_skill_context(*skill);
    
    if (activated) {
        result["message"] = "Skill '" + skill_name + "' loaded and activated.";
    } else {
        result["message"] = "Skill '" + skill_name + "' loaded but not activated (max active skills reached).";
    }
    
    return result;
}

// ============================================================================
// GetActiveSkillsTool
// ============================================================================

GetActiveSkillsTool::GetActiveSkillsTool() {
    name_ = "get_active_skills";
    description_ = "List the skills currently active in the agent context.";
}

json GetActiveSkillsTool::get_definition() const {
    return {
        {"type", "function"},
        {"function", {
            {"name", get_name()},
            {"description", "Returns a list of currently active (loaded) skills. Active skills are automatically included in the agent's context."},
            {"parameters", {
                {"type", "object"},
                {"properties", json::object()},
                {"required", json::array()}
            }}
        }}
    };
}

json GetActiveSkillsTool::execute(const json& /*args*/, FileIO& /*file_io*/) {
    SkillManager* skill_manager = SkillManager::current();
    if (!skill_manager) {
        return json::object({
            {"status", "error"},
            {"message", "SkillManager not initialized"}
        });
    }
    
    json result = json::object();
    result["status"] = "success";
    result["skills"] = json::array();
    
    auto active_skills = skill_manager->get_active_skills();
    for (const auto& skill : active_skills) {
        json skill_info = json::object();
        skill_info["name"] = skill.metadata.name;
        skill_info["description"] = skill.metadata.description;
        result["skills"].push_back(skill_info);
    }
    
    result["count"] = active_skills.size();
    
    return result;
}

// ============================================================================
// SearchSkillsTool
// ============================================================================

SearchSkillsTool::SearchSkillsTool() {
    name_ = "search_skills";
    description_ = "Search available skills by name or description.";
}

json SearchSkillsTool::get_definition() const {
    return {
        {"type", "function"},
        {"function", {
            {"name", get_name()},
            {"description", "Searches skills by query. Matches against skill names and descriptions."},
            {"parameters", {
                {"type", "object"},
                {"properties", {
                    {"query", {
                        {"type", "string"},
                        {"description", "Search query to match against skill names and descriptions"}
                    }}
                }},
                {"required", {"query"}}
            }}
        }}
    };
}

json SearchSkillsTool::execute(const json& args, FileIO& /*file_io*/) {
    SkillManager* skill_manager = SkillManager::current();
    if (!skill_manager) {
        return json::object({
            {"status", "error"},
            {"message", "SkillManager not initialized"}
        });
    }
    
    std::string query;
    if (args.contains("query")) {
        query = args["query"].get<std::string>();
    } else {
        return json::object({
            {"status", "error"},
            {"message", "Missing required parameter: query"}
        });
    }
    
    auto results = skill_manager->search_skills(query);
    
    json result = json::object();
    result["status"] = "success";
    result["query"] = query;
    result["results"] = json::array();
    
    for (const auto& skill : results) {
        json skill_info = json::object();
        skill_info["name"] = skill.metadata.name;
        skill_info["description"] = skill.metadata.description;
        skill_info["is_active"] = skill_manager->is_skill_active(skill.metadata.name);
        result["results"].push_back(skill_info);
    }
    
    result["count"] = results.size();
    
    return result;
}

// ============================================================================
// DeactivateSkillTool
// ============================================================================

DeactivateSkillTool::DeactivateSkillTool() {
    name_ = "deactivate_skill";
    description_ = "Deactivate a skill by name, removing it from the active context.";
}

json DeactivateSkillTool::get_definition() const {
    return {
        {"type", "function"},
        {"function", {
            {"name", get_name()},
            {"description", "Deactivates a skill by name, removing it from the active context."},
            {"parameters", {
                {"type", "object"},
                {"properties", {
                    {"skill_name", {
                        {"type", "string"},
                        {"description", "Name of the skill to deactivate"}
                    }}
                }},
                {"required", {"skill_name"}}
            }}
        }}
    };
}

json DeactivateSkillTool::execute(const json& args, FileIO& /*file_io*/) {
    SkillManager* skill_manager = SkillManager::current();
    if (!skill_manager) {
        return json::object({
            {"status", "error"},
            {"message", "SkillManager not initialized"}
        });
    }
    
    std::string skill_name;
    if (args.contains("skill_name")) {
        skill_name = args["skill_name"].get<std::string>();
    } else {
        return json::object({
            {"status", "error"},
            {"message", "Missing required parameter: skill_name"}
        });
    }
    
    bool deactivated = skill_manager->deactivate_skill(skill_name);
    
    json result = json::object();
    result["status"] = "success";
    result["skill_name"] = skill_name;
    result["deactivated"] = deactivated;
    
    if (deactivated) {
        result["message"] = "Skill '" + skill_name + "' deactivated.";
    } else {
        result["message"] = "Skill '" + skill_name + "' was not active.";
    }
    
    return result;
}

// ============================================================================
// GetSkillContextTool
// ============================================================================

GetSkillContextTool::GetSkillContextTool() {
    name_ = "get_skill_context";
    description_ = "Show the combined instructions from all active skills.";
}

json GetSkillContextTool::get_definition() const {
    return {
        {"type", "function"},
        {"function", {
            {"name", get_name()},
            {"description", "Returns the combined context from all active skills. This shows the instructions that will be included in the agent's context."},
            {"parameters", {
                {"type", "object"},
                {"properties", json::object()},
                {"required", json::array()}
            }}
        }}
    };
}

json GetSkillContextTool::execute(const json& /*args*/, FileIO& /*file_io*/) {
    SkillManager* skill_manager = SkillManager::current();
    if (!skill_manager) {
        return json::object({
            {"status", "error"},
            {"message", "SkillManager not initialized"}
        });
    }
    
    std::string context = skill_manager->get_active_skills_context();
    
    json result = json::object();
    result["status"] = "success";
    result["context"] = context;
    result["has_context"] = !context.empty();
    
    return result;
}
