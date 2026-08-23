#pragma once

#include <daedalus.h>

#include <string>

class AgentStore;
class ThreadStore;
class AgentRegistry;

/**
 * Collaboration tools registered on every roster agent (HiveMind 2.0,
 * Plan.md Phase 3). These are what make a "chief of staff" pattern
 * possible: an orchestrator agent discovers specialists, messages them,
 * and spins up new teammates — all through ordinary tool calls.
 */

/// list_agents — roster discovery (names, roles, ids).
class ListAgentsTool final : public daedalus::BaseTool {
public:
    explicit ListAgentsTool(AgentStore& agents) : agents_(agents) {}
    [[nodiscard]] std::string get_name() const override { return "list_agents"; }
    [[nodiscard]] std::string get_description() const override {
        return "List the other agents on the team (id, name, role).";
    }
    [[nodiscard]] nlohmann::json get_definition() const override;
    nlohmann::json execute(const nlohmann::json& args,
                           daedalus::HostToolContext& ctx) override;
private:
    AgentStore& agents_;
};

/// message_agent — send a task/question to a teammate and get their reply.
/// Synchronous: runs the target agent's turn inline (nested under the
/// caller's turn; depth-limited to prevent delegation loops).
class MessageAgentTool final : public daedalus::BaseTool {
public:
    MessageAgentTool(std::string sender_id, AgentStore& agents,
                     AgentRegistry& registry)
        : sender_id_(std::move(sender_id)), agents_(agents), registry_(registry) {}
    [[nodiscard]] std::string get_name() const override { return "message_agent"; }
    [[nodiscard]] std::string get_description() const override {
        return "Send a message to a teammate agent (by name or id) and wait "
               "for their reply. Use for delegation and coordination.";
    }
    [[nodiscard]] nlohmann::json get_definition() const override;
    nlohmann::json execute(const nlohmann::json& args,
                           daedalus::HostToolContext& ctx) override;

    /// Max nesting depth for agent→agent turns (loop protection).
    static constexpr int kMaxDepth = 4;
    /// Current depth on this thread (run_turn increments it).
    static thread_local int depth_;

private:
    std::string sender_id_;
    AgentStore& agents_;
    AgentRegistry& registry_;
};

/// create_agent — spin up a new named teammate on the roster.
class CreateAgentTool final : public daedalus::BaseTool {
public:
    explicit CreateAgentTool(AgentStore& agents) : agents_(agents) {}
    [[nodiscard]] std::string get_name() const override { return "create_agent"; }
    [[nodiscard]] std::string get_description() const override {
        return "Create a new teammate agent with a name, role and optional "
               "model. Returns the new agent's id.";
    }
    [[nodiscard]] nlohmann::json get_definition() const override;
    nlohmann::json execute(const nlohmann::json& args,
                           daedalus::HostToolContext& ctx) override;
private:
    AgentStore& agents_;
};

/// run_icarus — delegate a heavy duty task to an icarus subprocess
/// (our reconfigurable agent host). Graceful error when icarus isn't
/// installed on this machine.
class RunIcarusTool final : public daedalus::BaseTool {
public:
    RunIcarusTool(std::string agent_id, std::string workdir)
        : agent_id_(std::move(agent_id)), workdir_(std::move(workdir)) {}
    [[nodiscard]] std::string get_name() const override { return "run_icarus"; }
    [[nodiscard]] std::string get_description() const override {
        return "Delegate a self-contained task to an icarus agent subprocess "
               "(autonomous worker with its own tools) and return its output.";
    }
    [[nodiscard]] nlohmann::json get_definition() const override;
    nlohmann::json execute(const nlohmann::json& args,
                           daedalus::HostToolContext& ctx) override;
private:
    std::string agent_id_;
    std::string workdir_;
};
