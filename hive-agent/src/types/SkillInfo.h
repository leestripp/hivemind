/**
 * @file hive-brain/SkillInfo.h
 * @brief Read-only description of a skill the agent has discovered.
 */
#pragma once

#include <string>
#include <vector>

namespace hive {

struct SkillInfo {
    std::string name;
    std::string description;
    std::string source_path;       ///< File the skill was loaded from
    std::vector<std::string> tags;
};

}  // namespace hive