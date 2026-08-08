// src/chat/ChatAgent.cpp — the server's long-lived HiveMind assistant.

#include "hivemind/chat/ChatAgent.h"
#include "hivemind/chat/HivemindTools.h"

#include <filesystem>
#include <iostream>
#include <thread>

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {

const char* kSystemPrompt = R"(You are the HiveMind Assistant — an expert at building HiveMind workflows.

HiveMind is a visual workflow editor: a workflow is a directed graph of small
nodes wired together with edges that carry text. When the workflow runs, each
node fires when its inputs are ready.

NODE TYPES (ports in parentheses):
- input (out): reads a key from the run inputs. config: {param_name, value?}
- prompt (out): emits static text. config: {value}
- agent (in → out): an AI agent like yourself; reasons, calls tools, streams.
  config: {model, project?, system_prompt?}
- merge (in_a, in_b, in_c → out): joins multiple texts. config: {strategy?, separator?}
- loop_start (in → out, loop_count) + loop_end (in, loop_count → out): repeat the
  nodes between them loop_count times. loop_end's body text feeds back into
  loop_start's input. Wire loop_start.loop_count → loop_end.loop_count.
- output (in): collects a named result. config: {output_name}
- log (in): writes to the server log.

RULES FOR BUILDING:
1. Call hivemind_list_node_types if unsure about ports or config fields.
2. Every connection is source_node:out → target_node:in (defaults), except
   merge inputs (in_a/in_b/in_c) and the loop pair's loop_count port.
3. A runnable workflow usually ends at an output node. Workflows with an
   input node can receive run inputs keyed by the input node's param_name.
4. For the agent node, default model is "MiniMax-M2.7" unless the user asks
   for another. Set project to a real directory if the agent must touch files.
5. Triggers fire workflows: schedule kinds once/daily/weekly/interval or a
   webhook URL. Times are server-local.
6. After building or changing something, briefly summarize what you did —
   mention the workflow/trigger names so the user can find them in the UI.
7. If a tool returns an error, fix the arguments and retry once; then explain.

Be concise in chat. Build exactly what the user asks; ask a short clarifying
question only if a choice genuinely matters.
)";

// Sink that captures assistant text + a brief tool activity log per turn.
class ChatSink final : public daedalus::AgentSink {
public:
    void on_assistant_delta(std::string_view text) noexcept override {
        std::lock_guard<std::mutex> lk(m_);
        text_ += text;
    }
    void on_tool_call(std::string_view name, std::string_view,
                      const nlohmann::json&) noexcept override {
        std::lock_guard<std::mutex> lk(m_);
        tool_log_ += "⚙ " + std::string(name) + "\n";
    }
    void on_error(daedalus::ErrorKind, std::string_view msg, bool) noexcept override {
        std::lock_guard<std::mutex> lk(m_);
        errored_ = true;
        tool_log_ += "⚠ " + std::string(msg) + "\n";
    }
    bool request_command_approval(std::string_view) override { return true; }

    std::string take_text()     { std::lock_guard<std::mutex> lk(m_); return std::move(text_); }
    std::string take_tool_log() { std::lock_guard<std::mutex> lk(m_); return std::move(tool_log_); }
    bool errored() const { return errored_.load(); }
private:
    std::mutex m_;
    std::string text_, tool_log_;
    std::atomic<bool> errored_{false};
};

ChatAgent* g_instance = nullptr;

}  // namespace

ChatAgent::ChatAgent(GraphStore& store, ExecutorRegistry& registry)
    : store_(store), registry_(registry), sink_(std::make_shared<ChatSink>()) {}

ChatAgent& chat_agent(GraphStore* store, ExecutorRegistry* registry) {
    if (!g_instance) {
        if (!store || !registry)
            throw std::runtime_error("chat_agent: not initialized");
        g_instance = new ChatAgent(*store, *registry);
    }
    return *g_instance;
}

void ChatAgent::ensure_agent() {
    if (agent_) return;

    daedalus::AgentConfig cfg;
    cfg.model_name   = model_name_;                 // user-selectable via Chat page
    cfg.session_id   = "hivemind-chat-" + std::to_string(session_gen_);
    cfg.project_root = fs::current_path().string();
    cfg.system_prompt = kSystemPrompt;
    cfg.auto_approve  = true;
    cfg.mcp_enabled   = false;                   // faster init; hivemind tools only
    cfg.tools_disabled = {"execute_bash", "app_ui_control"};  // builder agent

    agent_ = std::make_unique<daedalus::Agent>(std::move(cfg), sink_);
    for (auto& t : make_hivemind_tools(store_, registry_))
        agent_->register_tool(std::move(t));
    agent_->initialize();
}

void ChatAgent::set_model(const std::string& model) {
    if (model.empty() || model == model_name_) return;
    std::lock_guard<std::mutex> lk(m_);
    if (pending_) return;                    // don't yank the rug mid-turn
    model_name_ = model;
    agent_.reset();                          // fresh agent on next turn
    ++session_gen_;
}

std::string ChatAgent::current_model() const {
    std::lock_guard<std::mutex> lk(m_);
    return model_name_;
}

bool ChatAgent::send_async(const std::string& message) {
    {
        std::lock_guard<std::mutex> lk(m_);
        if (pending_) return false;
        pending_ = true;
        history_.push_back({{"role", "user"}, {"content", message}});
    }
    std::thread([this, message]() { run_turn(message); }).detach();
    return true;
}

void ChatAgent::run_turn(std::string message) {
    json reply{{"role", "assistant"}};
    try {
        {
            std::lock_guard<std::mutex> lk(m_);
            ensure_agent();
        }
        auto sink = std::static_pointer_cast<ChatSink>(sink_);
        sink->take_text();       // clear per-turn buffers
        sink->take_tool_log();

        agent_->submit_user_message(message);
        agent_->run_one_turn();

        std::string text = sink->take_text();
        std::string log  = sink->take_tool_log();
        reply["content"]  = text.empty() ? "(no reply)" : text;
        if (!log.empty()) reply["tool_log"] = log;
    } catch (const std::exception& e) {
        reply["content"] = std::string("turn failed: ") + e.what();
    }
    {
        std::lock_guard<std::mutex> lk(m_);
        history_.push_back(std::move(reply));
        pending_ = false;
    }
}

json ChatAgent::history() const {
    std::lock_guard<std::mutex> lk(m_);
    return history_;
}

bool ChatAgent::pending() const {
    std::lock_guard<std::mutex> lk(m_);
    return pending_;
}

void ChatAgent::reset() {
    std::lock_guard<std::mutex> lk(m_);
    if (pending_) return;    // don't yank the agent mid-turn
    agent_.reset();          // fresh session on next message
    history_.clear();
    ++session_gen_;
}
