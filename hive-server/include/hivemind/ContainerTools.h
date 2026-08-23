#pragma once

#include <daedalus.h>

#include <functional>
#include <string>

/**
 * ContainerBashTool — container-backed replacement for daedalus' built-in
 * `execute_bash` (HiveMind 2.0, Plan.md Phase 2).
 *
 * For container-bound roster agents, the built-in host-side execute_bash is
 * disabled (AgentConfig::tools_disabled) and this tool is registered as
 * `container_exec`. (It can't reuse the built-in's name: tools_disabled hides
 * host-registered tools with the same name too.)
 * Commands run as user `hive` in /workspace inside the agent's podman
 * container, where the agent has passwordless sudo to install CLI tools.
 *
 * If the container runtime is unavailable the tool returns a JSON error
 * (status:"error") instead of throwing, so the agent can report it.
 */
class ContainerBashTool final : public daedalus::BaseTool {
public:
    explicit ContainerBashTool(std::string agent_id) : agent_id_(std::move(agent_id)) {}

    /// Optional approval hook (Phase 3): when set, every command is parked
    /// in the approval queue first and only runs if the hook returns true.
    void set_approval_fn(std::function<bool(std::string_view)> fn) {
        approval_fn_ = std::move(fn);
    }

    [[nodiscard]] std::string get_name() const override { return "container_exec"; }

    [[nodiscard]] std::string get_description() const override {
        return "Execute a bash command inside your Linux container as user "
               "'hive' (passwordless sudo available; working dir /workspace).";
    }

    [[nodiscard]] nlohmann::json get_definition() const override {
        return {
            {"type", "function"},
            {"function", {
                {"name", get_name()},
                {"description", get_description()},
                {"parameters", {
                    {"type", "object"},
                    {"properties", {
                        {"command", {
                            {"type", "string"},
                            {"description", "The bash command to run."},
                        }},
                        {"timeout", {
                            {"type", "integer"},
                            {"description", "Timeout in seconds (default 120)."},
                        }},
                    }},
                    {"required", nlohmann::json::array({"command"})},
                }},
            }},
        };
    }

    nlohmann::json execute(const nlohmann::json& args,
                           daedalus::HostToolContext& ctx) override;

private:
    std::string agent_id_;
    std::function<bool(std::string_view)> approval_fn_;
};
