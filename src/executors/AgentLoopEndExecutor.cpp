#include "hivemind/executors/AgentLoopEndExecutor.h"
#include "hivemind/Graph.h"
#include <daedalus.h>
#include <atomic>
#include <filesystem>
#include <memory>
#include <mutex>
#include <stdexcept>

namespace fs = std::filesystem;
namespace {

struct LoopDecision {
    std::mutex m;
    bool        decided   = false;
    std::string decision  = "exit";
    std::string feedback;
};

class LoopDecisionTool final : public daedalus::BaseTool {
public:
    explicit LoopDecisionTool(std::shared_ptr<LoopDecision> st) : st_(std::move(st)) {}
    std::string get_name() const override { return "loop_decision"; }
    std::string get_description() const override {
        return "Decide whether the loop continues another iteration or exits. "
               "Call exactly once per review.";
    }
    nlohmann::json get_definition() const override {
        return {
            {"type", "function"},
            {"function", {
                {"name", "loop_decision"},
                {"description", get_description()},
                {"parameters", {
                    {"type", "object"},
                    {"properties", {
                        {"decision", {{"type","string"},
                                      {"enum", nlohmann::json::array({"continue","exit"})},
                                      {"description","'continue' to run another loop iteration, 'exit' to finish the loop"}}},
                        {"feedback", {{"type","string"},
                                      {"description","On continue: guidance sent back to the loop start for the next iteration. On exit: the final result text."}}}
                    }},
                    {"required", nlohmann::json::array({"decision","feedback"})},
                }},
            }},
        };
    }
    nlohmann::json execute(const nlohmann::json& args,
                           daedalus::HostToolContext&) override {
        std::lock_guard<std::mutex> lk(st_->m);
        st_->decided  = true;
        st_->decision = args.value("decision", "exit");
        st_->feedback = args.value("feedback", "");
        if (st_->decision != "continue") st_->decision = "exit";
        return {{"status","ok"},{"decision", st_->decision}};
    }
private:
    std::shared_ptr<LoopDecision> st_;
};

class QuietSink final : public daedalus::AgentSink {
public:
    void on_assistant_delta(std::string_view t) noexcept override {
        std::lock_guard<std::mutex> lk(m_);
        text_ += t;
    }
    void on_error(daedalus::ErrorKind, std::string_view msg, bool) noexcept override {
        errored_ = true; err_ = msg;
    }
    bool request_command_approval(std::string_view) override { return true; }
    bool errored() const { return errored_.load(); }
    std::string err() const { return err_; }
    std::string text() const { std::lock_guard<std::mutex> lk(m_); return text_; }
private:
    mutable std::mutex m_;
    std::string text_;
    std::atomic<bool> errored_{false};
    std::string err_;
};

} // namespace

std::vector<NodeResult>
AgentLoopEndExecutor::execute(const Node& node,
    const std::unordered_map<std::string, std::string>& inputs,
    const std::string& run_id)
{
    std::string body   = inputs.count("in")        ? inputs.at("in")        : std::string{};
    std::string instr  = inputs.count("prompt_in") ? inputs.at("prompt_in") : std::string{};

    std::string model;
    if (node.config_json.contains("model") && node.config_json["model"].is_string())
        model = node.config_json["model"].get<std::string>();

    daedalus::AgentConfig cfg;
    cfg.model_name   = model;
    cfg.session_id   = run_id + "-" + node.id;
    // Judge can be pointed at a project dir (e.g. to inspect the worker's
    // output files); default cwd.
    std::string project;
    if (node.config_json.contains("project") && node.config_json["project"].is_string())
        project = node.config_json["project"].get<std::string>();
    cfg.project_root = (!project.empty() && fs::exists(project))
                           ? project : fs::current_path().string();
    cfg.auto_approve = true;
    cfg.mcp_enabled  = false;
    cfg.max_agent_iterations = 6;   // bound flailing; one tool call is enough
    cfg.system_prompt =
        "You are the judge of a workflow loop. Read the input and deliver your "
        "verdict by calling the loop_decision tool exactly once "
        "(decision='continue' or 'exit', plus feedback). Do not do any other work.";

    auto state = std::make_shared<LoopDecision>();
    auto sink  = std::make_shared<QuietSink>();
    daedalus::Agent agent{std::move(cfg), sink};
    agent.register_tool(std::make_unique<LoopDecisionTool>(state));
    agent.initialize();

    std::string prompt =
        "You are the judge of a workflow loop. Review the loop body's latest "
        "output and decide: should the loop run another iteration, or exit?\n\n"
        "Instructions from the workflow author:\n" + (instr.empty() ? "(none)" : instr) +
        "\n\nLatest loop body output:\n" + (body.empty() ? "(empty)" : body) +
        "\n\nCall loop_decision exactly once. Use 'continue' only if another "
        "iteration would materially improve the result.";
    agent.submit_user_message(prompt);
    agent.run_one_turn();

    if (sink->errored())
        throw std::runtime_error{"agent_loop_end judge error: " + sink->err()};

    std::string decision, feedback;
    {
        std::lock_guard<std::mutex> lk(state->m);
        decision = state->decided ? state->decision : "exit";
        feedback = state->feedback;
    }
    if (decision == "exit" && feedback.empty()) {
        // Tool never called (weak model): fall back to parsing the text.
        std::string t = sink->text();
        std::string low = t;
        for (auto& c : low) c = (char)tolower((unsigned char)c);
        if (low.find("continue") != std::string::npos
            && low.find("exit") == std::string::npos) {
            decision = "continue";
            feedback = t;
        } else {
            feedback = t.empty() ? feedback : t;
        }
    }
    if (decision == "continue")
        return {NodeResult{node.id, "__continue__", feedback}};
    return {NodeResult{node.id, "out", feedback.empty() ? body : feedback}};
}
