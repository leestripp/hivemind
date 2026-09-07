#pragma once

#include <string>
#include <nlohmann/json.hpp>
#include "BaseTool.h"

class FileIO;

using json = nlohmann::json;

/**
 * @file SetStatusTool.h
 * @brief §State Machine: tools that let the *agent* drive its own
 *        state transitions.
 *
 * The library's runtime infers most state transitions from the model's
 * actions (a tool call → Acting; execute_bash → Verifying; etc.).  But
 * the agent is the only one that knows when work is truly done — and
 * when verification fails and it must loop back to Planning.  This
 * tool exposes the public `AgentState` enum so the model can drive
 * transitions explicitly.
 *
 * After the tool runs, `LLMClient::run_agent_cycle()` inspects the
 * result; when `status == "set_state"` it calls `sink_->on_state_change`
 * with the new public state so the host UI / SSE stream see the
 * transition immediately.
 */
class SetStatusTool : public BaseTool {
public:
    std::string get_name() const override { return "set_status"; }
    std::string get_description() const override {
        return "Update the agent's current state in its state machine (Understanding, Planning, "
               "Acting, Verifying, Documenting, etc.).  Call this as you move through the workflow "
               "so the host UI / sink can render the current stage correctly.  YOU are the only one "
               "who knows when verification failed and you must loop back to Understanding or "
               "Planning — call this to reflect that.  Use Documenting when you are updating "
               "README, CHANGELOG, comments, or tool docs after the code change is verified.  "
               "NOTE: set_status only updates the host UI stage — it NEVER exits the agent "
               "loop, not even Done.  To end your turn, call the stop tool.";
    }
    json get_definition() const override;
    json execute(const json& args, FileIO& file_io) override;
};
