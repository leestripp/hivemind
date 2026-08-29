#pragma once

#include <daedalus.h>

#include <string>

class AgentStore;
class ThreadStore;
class RoutineStore;
class AgentRegistry;
class WorkerStore;
class WorkerChannel;

namespace agent_tools {
/// Phase 30: give the collaboration tools visibility of enrolled hive-workers
/// (list_workers / create_agent's worker param). Called once from main() next
/// to WorkerRouter::install — same global-access pattern.
void install_worker_access(WorkerStore* workers, WorkerChannel* channel);
}  // namespace agent_tools

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
        return "List EVERY agent in the company — the full staff directory "
               "(id, name, role), including yourself. Always use this to "
               "look up a teammate's exact name before contacting them.";
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
        return "Send a message to a teammate agent (by name or id — use "
               "list_agents to find them). This is the ONLY way to contact "
               "another agent: use it to introduce yourself, say hello, "
               "delegate a task, or ask a question. The message is posted "
               "to your shared thread and delivered asynchronously: the "
               "reply arrives later as a new message to you. Do NOT wait "
               "for an immediate answer; continue your own work or report "
               "what you sent.";
    }
    [[nodiscard]] nlohmann::json get_definition() const override;
    nlohmann::json execute(const nlohmann::json& args,
                           daedalus::HostToolContext& ctx) override;

private:
    std::string sender_id_;
    AgentStore& agents_;
    AgentRegistry& registry_;
};

/// send_file — deliver a file from the agent's /workspace into the
/// company workspace on the hive-server (Phase 31). For agents on
/// hive-worker rigs the bytes ride the worker channel; for local agents
/// it is a confined copy inside the shared drive. Destination is a
/// relative path under the company workspace root (parent must exist);
/// overwrite requires overwrite=true. Optional 'notify' posts a note to
/// a teammate's shared thread.
class SendFileTool final : public daedalus::BaseTool {
public:
    SendFileTool(std::string sender_id, AgentStore& agents,
                 AgentRegistry& registry)
        : sender_id_(std::move(sender_id)), agents_(agents), registry_(registry) {}
    [[nodiscard]] std::string get_name() const override { return "send_file"; }
    [[nodiscard]] std::string get_description() const override {
        return "Send a file from your /workspace to the company workspace on "
               "the hive-server. Use this to deliver finished work (reports, "
               "builds, designs) so teammates on other machines can see it. "
               "'path' is the source file in YOUR workspace, 'dest' is where "
               "it lands in the company workspace (e.g. "
               "'departments/engineering/report.md' — the folder must "
               "already exist). Existing files are NOT overwritten unless "
               "overwrite=true. Max 50 MB. Optionally 'notify' a teammate "
               "by name — they get a thread message saying the file arrived.";
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
               "model. Optional 'worker' places it on an enrolled hive-worker "
               "rig (use list_workers to see them); default is 'local' — the "
               "server's own machine. Returns the new agent's id.";
    }
    [[nodiscard]] nlohmann::json get_definition() const override;
    nlohmann::json execute(const nlohmann::json& args,
                           daedalus::HostToolContext& ctx) override;
private:
    AgentStore& agents_;
};

namespace hivemind {

/// search_skills — keyword search over the skill marketplace registry
/// (web/dist/skills-registry.json). Agents should ALWAYS try this before
/// web-searching for a capability.
class SearchSkillsTool final : public daedalus::BaseTool {
public:
    SearchSkillsTool() = default;
    [[nodiscard]] std::string get_name() const override { return "search_skills"; }
    [[nodiscard]] std::string get_description() const override {
        return "Search the skill marketplace for installable skills by keyword "
               "(matches name, description, and tags). Returns each match's "
               "name, description, tags, and whether it is already installed. "
               "Use this FIRST when you need a new capability (e.g. 'remotion', "
               "'video', 'pdf'), then call install_skill with the skill name.";
    }
    [[nodiscard]] nlohmann::json get_definition() const override;
    nlohmann::json execute(const nlohmann::json& args,
                           daedalus::HostToolContext& ctx) override;
};

/// install_skill — install a marketplace skill (by registry name, or an
/// explicit git_url+path) into the global library AND enable it for the
/// calling agent (symlink into ~/.agents/skills/).
class InstallSkillTool final : public daedalus::BaseTool {
public:
    explicit InstallSkillTool(std::string agent_id)
        : agent_id_(std::move(agent_id)) {}
    [[nodiscard]] std::string get_name() const override { return "install_skill"; }
    [[nodiscard]] std::string get_description() const override {
        return "Install a skill from the marketplace (pass its 'name' from "
               "search_skills) or from an explicit git_url (+ optional 'path' "
               "subdir for monorepos). The skill lands in the shared library "
               "and is enabled for YOU immediately. Fails if already installed "
               "or if you have a same-named local skill.";
    }
    [[nodiscard]] nlohmann::json get_definition() const override;
    nlohmann::json execute(const nlohmann::json& args,
                           daedalus::HostToolContext& ctx) override;
private:
    std::string agent_id_;
};


}  // namespace hivemind

/// list_workers — Phase 30: discover enrolled hive-worker rigs (name,
/// online status, capabilities) so an orchestrator can decide where to
/// place a new teammate with create_agent's worker param.
class ListWorkersTool final : public daedalus::BaseTool {
public:
    [[nodiscard]] std::string get_name() const override { return "list_workers"; }
    [[nodiscard]] std::string get_description() const override {
        return "List enrolled hive-worker rigs — remote machines that can "
               "run agent containers — with name, online status and "
               "capabilities (gpu, arch). Pass a worker's name as 'worker' "
               "to create_agent to place the new teammate on that rig. "
               "Only ONLINE workers can actually start a container.";
    }
    [[nodiscard]] nlohmann::json get_definition() const override;
    nlohmann::json execute(const nlohmann::json& args,
                           daedalus::HostToolContext& ctx) override;
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

/// list_routines — the calling agent's own scheduled tasks (routines are
/// per-agent; an agent manages only its own).
class ListRoutinesTool final : public daedalus::BaseTool {
public:
    explicit ListRoutinesTool(std::string agent_id, RoutineStore& routines)
        : agent_id_(std::move(agent_id)), routines_(routines) {}
    [[nodiscard]] std::string get_name() const override { return "list_routines"; }
    [[nodiscard]] std::string get_description() const override {
        return "List your scheduled routines (id, name, schedule, enabled, "
               "next fire time). Use the id with update_routine / "
               "delete_routine.";
    }
    [[nodiscard]] nlohmann::json get_definition() const override;
    nlohmann::json execute(const nlohmann::json& args,
                           daedalus::HostToolContext& ctx) override;
private:
    std::string agent_id_;
    RoutineStore& routines_;
};

/// update_routine — edit name/prompt/schedule (and enable/disable) of one
/// of the calling agent's own routines. next_fire_at is recomputed when
/// the schedule changes (same policy as RoutineRoutes PATCH).
class UpdateRoutineTool final : public daedalus::BaseTool {
public:
    explicit UpdateRoutineTool(std::string agent_id, RoutineStore& routines)
        : agent_id_(std::move(agent_id)), routines_(routines) {}
    [[nodiscard]] std::string get_name() const override { return "update_routine"; }
    [[nodiscard]] std::string get_description() const override {
        return "Edit one of your routines: rename, change the prompt, "
               "reschedule, or pause/resume it (enabled:false/true). Only "
               "routines you own can be changed.";
    }
    [[nodiscard]] nlohmann::json get_definition() const override;
    nlohmann::json execute(const nlohmann::json& args,
                           daedalus::HostToolContext& ctx) override;
private:
    std::string agent_id_;
    RoutineStore& routines_;
};

/// delete_routine — permanently remove one of the calling agent's own
/// routines.
class DeleteRoutineTool final : public daedalus::BaseTool {
public:
    explicit DeleteRoutineTool(std::string agent_id, RoutineStore& routines)
        : agent_id_(std::move(agent_id)), routines_(routines) {}
    [[nodiscard]] std::string get_name() const override { return "delete_routine"; }
    [[nodiscard]] std::string get_description() const override {
        return "Delete one of your routines permanently. Only routines you "
               "own can be deleted.";
    }
    [[nodiscard]] nlohmann::json get_definition() const override;
    nlohmann::json execute(const nlohmann::json& args,
                           daedalus::HostToolContext& ctx) override;
private:
    std::string agent_id_;
    RoutineStore& routines_;
};

