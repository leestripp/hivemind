#pragma once

// AgentTools (HiveMind 2.0 Phase C): the hive-brain host Tool classes are
// GONE (brains run in hive-agent on workers now). What remains is the
// worker skill-sync plumbing used by SkillRoutes / RosterRoutes / main.

#include <string>
#include <vector>

struct RosterAgent;
class WorkerStore;
class WorkerChannel;

namespace agent_tools {
/// Give the skill-sync helpers visibility of enrolled hive-workers.
/// Called once from main() next to WorkerRouter::install.
void install_worker_access(WorkerStore* workers, WorkerChannel* channel);

/// Push an enabled global skill to the agent's worker rig
/// (no-op for offline workers / missing skill).
void push_skill_to_worker(const RosterAgent& agent, const std::string& skill);
void remove_skill_on_worker(const RosterAgent& agent, const std::string& skill);
/// Global skills currently enabled for an agent (symlink scan).
std::vector<std::string> list_enabled_skills(const std::string& agent_id);
}  // namespace agent_tools
