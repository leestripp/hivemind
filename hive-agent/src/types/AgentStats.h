/**
 * @file hive-brain/AgentStats.h
 * @brief Snapshot of agent run statistics. Pure data; hosts format for display.
 */
#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace hive {

struct AgentStats {
    int  iteration = 0;
    int  max_iterations = 0;
    int  prompt_tokens = 0;
    int  completion_tokens = 0;
    int  total_tokens = 0;
    // Prompt tokens served from the provider's server-side context cache
    // on the last response (0 = miss or provider doesn't report it).
    int  cached_tokens = 0;
    int  max_context_size = 0;
    int  message_count = 0;
    int  pending_tool_calls = 0;
    int  executing_tool_calls = 0;
    int  completed_tool_calls = 0;
    int  failed_tool_calls = 0;

    std::vector<std::string> active_skill_names;
    std::vector<std::string> loaded_mcp_servers;
};

}  // namespace hive