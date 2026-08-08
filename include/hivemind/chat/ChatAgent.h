#pragma once

/**
 * ChatAgent — the server's long-lived HiveMind assistant.
 *
 * Owns one daedalus::Agent configured with the HiveMind-control tool set
 * (see HivemindTools.h) and a builder system prompt. One chat turn runs at
 * a time (mutex). The message history is kept here as simple JSON so the
 * Chat page can poll it — the agent itself keeps its own conversation in
 * its daedalus session.
 *
 * Thread-safety: all public methods lock an internal mutex. Never call
 * from an executor thread while holding GraphRunner state.
 */

#include <daedalus.h>
#include <nlohmann/json.hpp>

#include <memory>
#include <mutex>
#include <string>
#include <vector>

class GraphStore;
class ExecutorRegistry;

class ChatAgent {
public:
    ChatAgent(GraphStore& store, ExecutorRegistry& registry);

    /// Start a turn with `message` on a background thread. Returns false
    /// if a turn is already in flight.
    bool send_async(const std::string& message);

    /// Full chat history as [{role:"user"|"assistant", content, tool_log?}].
    nlohmann::json history() const;

    /// True while a turn is running.
    bool pending() const;

    /// Clear history and start a fresh agent session on the next message.
    void reset();

    /// Switch the model used for NEW turns. Changing the model discards the
    /// current agent (fresh session — model is baked into AgentConfig).
    void set_model(const std::string& model);

    /// The model new turns will use ("" = daedalus SettingsStore default).
    std::string current_model() const;

private:
    void ensure_agent();          // construct agent if needed (caller holds lock)
    void run_turn(std::string message);  // worker thread body

    GraphStore&       store_;
    ExecutorRegistry& registry_;

    mutable std::mutex                    m_;
    std::shared_ptr<daedalus::AgentSink>  sink_;
    std::unique_ptr<daedalus::Agent>      agent_;
    std::vector<nlohmann::json>      history_;
    bool                             pending_ = false;
    int                              session_gen_ = 0;  // bumped on reset
    std::string                      model_name_ = "MiniMax-M2.7";  // verified live default
};

/// Global singleton — constructed once from main().
ChatAgent& chat_agent(GraphStore* store = nullptr, ExecutorRegistry* registry = nullptr);
