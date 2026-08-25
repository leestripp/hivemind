#pragma once

#include <daedalus.h>

#include <string>

class AgentStore;
class ThreadStore;
class RoutineStore;
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
        return "Send a message to a teammate agent (by name or id). The "
               "message is posted to your shared thread and delivered "
               "asynchronously: the reply arrives later as a new message "
               "to you. Do NOT wait for an immediate answer; continue your "
               "own work or report what you sent.";
    }
    [[nodiscard]] nlohmann::json get_definition() const override;
    nlohmann::json execute(const nlohmann::json& args,
                           daedalus::HostToolContext& ctx) override;

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

/// save_routine — Phase 20 learn-by-demonstration: after the user
/// demonstrates a workflow in a thread ("watch me do this once"), the
/// agent distills it into a named, scheduled routine that then fires
/// on its own. Mirrors RoutineRoutes' create path (own thread per
/// routine, schedule validated, next_fire computed).
class SaveRoutineTool final : public daedalus::BaseTool {
public:
    SaveRoutineTool(std::string agent_id, RoutineStore& routines,
                    ThreadStore& threads)
        : agent_id_(std::move(agent_id)), routines_(routines),
          threads_(threads) {}
    [[nodiscard]] std::string get_name() const override { return "save_routine"; }
    [[nodiscard]] std::string get_description() const override {
        return "Save a demonstrated workflow as a recurring or one-shot "
               "routine that runs on a schedule. Use after the user shows "
               "you a task and asks you to learn/remember it. The prompt "
               "must be self-contained (steps, URLs, file paths, values) — "
               "future fires get no thread history.";
    }
    [[nodiscard]] nlohmann::json get_definition() const override;
    nlohmann::json execute(const nlohmann::json& args,
                           daedalus::HostToolContext& ctx) override;
private:
    std::string agent_id_;
    RoutineStore& routines_;
    ThreadStore& threads_;
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
