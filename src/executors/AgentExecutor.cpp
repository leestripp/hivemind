// =============================================================================
// src/executors/AgentExecutor.cpp
// -----------------------------------------------------------------------------
// AI Agent node: delegates text-in / text-out to an embedded
// daedalus::Agent. Constructed per execution (one Agent per agent-node
// execution, per design.md §6.2).
//
// The executor implements daedalus::AgentSink to stream agent events
// out as a single text payload on the node's "out" port. In a future
// pass we can split the events into separate ports / event types
// (see Plan.md §3.4 — sink→event mapping table).
// =============================================================================

#include "hivemind/executors/AgentExecutor.h"
#include "hivemind/Graph.h"

#include <daedalus.h>

#include <filesystem>
#include <memory>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string_view>

namespace fs = std::filesystem;

namespace {

std::string join_all(const std::unordered_map<std::string, std::string>& inputs) {
    std::ostringstream ss;
    for (auto& [k, v] : inputs) {
        if (!ss.str().empty()) ss << "\n";
        ss << v;
    }
    return ss.str();
}

// ─── CapturingSink ────────────────────────────────────────────────────────
// Aggregates agent events into a single text stream. Thread-safe (sink
// callbacks can fire on background threads inside daedalus).

class CapturingSink final : public daedalus::AgentSink {
public:
    // Default implementations of all `noexcept` callbacks — they
    // append to the buffer under a mutex.
    void on_assistant_delta(std::string_view text) noexcept override {
        if (text.empty()) return;
        std::lock_guard<std::mutex> lk(m_);
        // Trim long arg blobs in tool-call summaries
        output_ += text;
    }

    void on_tool_call(std::string_view name,
                      std::string_view /*call_id*/,
                      const nlohmann::json& args) noexcept override {
        std::lock_guard<std::mutex> lk(m_);
        output_ += "\n[tool: ";
        output_ += name;
        // Brief arg summary
        try {
            if (!args.is_null() && args.is_object()) {
                for (auto it = args.begin(); it != args.end(); ++it) {
                    if (it.value().is_string()) {
                        std::string s = it.value().get<std::string>();
                        if (s.size() > 80) s = s.substr(0, 77) + "...";
                        output_ += " ";
                        output_ += it.key();
                        output_ += "=";
                        output_ += s;
                    }
                }
            }
        } catch (...) {}
        output_ += "]\n";
    }

    void on_tool_result(std::string_view name,
                        std::string_view /*call_id*/,
                        const nlohmann::json& result) noexcept override {
        std::lock_guard<std::mutex> lk(m_);
        std::string r;
        try { r = result.dump(); } catch (...) {}
        if (r.size() > 500) r = r.substr(0, 497) + "...";
        output_ += "  → ";
        output_ += r;
        output_ += "\n";
        (void)name;
    }

    void on_state_change(int /*from*/, int to) noexcept override {
        std::lock_guard<std::mutex> lk(m_);
        const char* name = daedalus::to_string(
            static_cast<daedalus::AgentState>(to));
        if (name) {
            output_ += "\n[";
            output_ += name;
            output_ += "]\n";
        }
    }

    void on_error(daedalus::ErrorKind /*kind*/,
                  std::string_view msg,
                  bool will_retry) noexcept override {
        std::lock_guard<std::mutex> lk(m_);
        errored_ = true;
        output_ += "\n[error";
        if (will_retry) output_ += " (retrying)";
        output_ += "] ";
        output_ += msg;
        output_ += "\n";
    }

    void on_compaction(std::size_t before, std::size_t after) noexcept override {
        std::lock_guard<std::mutex> lk(m_);
        output_ += "\n[compaction ";
        output_ += std::to_string(before);
        output_ += " → ";
        output_ += std::to_string(after);
        output_ += " tokens]\n";
    }

    void on_log(daedalus::LogLevel /*lvl*/, std::string_view msg) noexcept override {
        // Library diagnostics — forward to stderr (mirrors prototype
        // behaviour and icarus CLI default).
        std::fwrite(msg.data(), 1, msg.size(), stderr);
        std::fputc('\n', stderr);
    }

    // Auto-approve shell commands in node-graph context. Hosts that
    // want approval UIs override this.
    bool request_command_approval(std::string_view /*cmd*/) override {
        return true;
    }

    // Read final state
    std::string consume_output() {
        std::lock_guard<std::mutex> lk(m_);
        return std::move(output_);
    }
    bool errored() const { return errored_.load(); }

private:
    std::mutex            m_;
    std::string           output_;
    std::atomic<bool>     errored_{false};
};

}  // namespace

std::vector<NodeResult>
AgentExecutor::execute(const Node& node,
                      const std::unordered_map<std::string, std::string>& inputs,
                      const std::string& run_id)
{
    // ── 1. Model selection ───────────────────────────────────────────────
    // daedalus owns model/provider settings (SQLite SettingsStore under
    // ~/.local/share/daedalus/). We pass the model name from the node
    // config straight through; empty means "use the store's default model".
    // Credentials/endpoints are resolved by the library — never here.
    std::string model_name;
    if (node.config_json.contains("model")
        && node.config_json["model"].is_string()) {
        model_name = node.config_json["model"].get<std::string>();
    }

    // ── 2. Build daedalus::AgentConfig ───────────────────────────────────
    daedalus::AgentConfig cfg;
    cfg.model_name         = model_name;
    // Each agent node must have its own session directory. Using the
    // run_id alone would make two parallel agents in the same run share
    // the same session path, which causes daedalus persistence conflicts
    // and can deadlock.
    cfg.session_id         = run_id + "-" + node.id;

    // Working directory: project path from node config, or cwd
    std::string project_path;
    if (node.config_json.contains("project")
        && node.config_json["project"].is_string()) {
        project_path = node.config_json["project"].get<std::string>();
    }
    if (project_path.empty() || !fs::exists(project_path)) {
        project_path = fs::current_path().string();
    }
    cfg.project_root = project_path;

    // System prompt override (optional)
    if (node.config_json.contains("system_prompt")
        && node.config_json["system_prompt"].is_string()) {
        cfg.system_prompt = node.config_json["system_prompt"].get<std::string>();
    }

    // Auto-approve (no user in node-graph context)
    cfg.auto_approve = true;

    // ── 3. Build sink + agent ────────────────────────────────────────────
    auto sink = std::make_shared<CapturingSink>();
    daedalus::Agent agent{std::move(cfg), sink};

    agent.initialize();

    // ── 4. Submit joined prompt and drive one turn ───────────────────────
    std::string task = join_all(inputs);
    if (task.empty()) task = "(no input)";

    agent.submit_user_message(task);
    agent.run_one_turn();    // returns on stop / cancel / error

    // ── 5. Capture output ─────────────────────────────────────────────────
    std::string output = sink->consume_output();
    if (output.empty()) output = "(agent produced no output)";

    if (sink->errored()) {
        // Don't throw — wrap as a partial result so the runner can
        // still emit graph_failed at the right level. But mark via
        // node failure event if you want: currently we throw to let
        // GraphRunner handle it.
        throw std::runtime_error{"agent reported errors (see captured output)"};
    }

    return {NodeResult{node.id, "out", std::move(output)}};
}

std::string_view AgentExecutor::type_name() const noexcept {
    return "agent";
}
