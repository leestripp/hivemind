#pragma once

#include "BaseTool.h"
#include <string>

/**
 * Base class for skill-related tools
 */
class BaseSkillTool : public BaseTool {
protected:
    std::string name_;
    std::string description_;
    
public:
    virtual ~BaseSkillTool() = default;
    virtual std::string get_name() const override { return name_; }
    virtual std::string get_description() const override { return description_; }
    virtual json get_definition() const override = 0;
    virtual json execute(const json& args, FileIO& file_io) override = 0;
};

/**
 * Tool: discover_skills
 * Lists all available skills with their metadata
 */
class DiscoverSkillsTool : public BaseSkillTool {
public:
    DiscoverSkillsTool();
    json get_definition() const override;
    json execute(const json& args, FileIO& file_io) override;
};

/**
 * Tool: load_skill
 * Loads a specific skill and adds it to active skills
 */
class LoadSkillTool : public BaseSkillTool {
public:
    LoadSkillTool();
    json get_definition() const override;
    json execute(const json& args, FileIO& file_io) override;
};

/**
 * Tool: get_active_skills
 * Returns currently active (loaded) skills
 */
class GetActiveSkillsTool : public BaseSkillTool {
public:
    GetActiveSkillsTool();
    json get_definition() const override;
    json execute(const json& args, FileIO& file_io) override;
};

/**
 * Tool: search_skills
 * Searches skills by query
 */
class SearchSkillsTool : public BaseSkillTool {
public:
    SearchSkillsTool();
    json get_definition() const override;
    json execute(const json& args, FileIO& file_io) override;
};

/**
 * Tool: deactivate_skill
 * Deactivates a skill by name
 */
class DeactivateSkillTool : public BaseSkillTool {
public:
    DeactivateSkillTool();
    json get_definition() const override;
    json execute(const json& args, FileIO& file_io) override;
};

/**
 * Tool: get_skill_context
 * Returns the combined context from all active skills
 */
class GetSkillContextTool : public BaseSkillTool {
public:
    GetSkillContextTool();
    json get_definition() const override;
    json execute(const json& args, FileIO& file_io) override;
};
