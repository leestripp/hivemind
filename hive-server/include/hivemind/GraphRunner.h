#pragma once

#include "Graph.h"
#include "LoopEngine.h"
#include <string>
#include <memory>
#include <vector>
#include <future>
#include <atomic>
#include <stop_token>
#include <unordered_set>
#include <queue>
#include <thread>
#include <mutex>

/**
 * GraphRunner — executes a graph from start to finish.
 *
 * Usage:
 *   GraphRunner runner(store, registry, graph, run_id, input_values);
 *   co_await runner.run(stoken);   // cancellable via stop_token
 *
 * Or blocking:
 *   GraphRunner runner(store, registry, graph, run_id, input_values);
 *   runner.run_sync();
 *
 * The runner walks the graph in topological order, executing nodes
 * concurrently (up to max_concurrent_ agents). Loop nodes cause
 * re-execution of the loop body.
 *
 * Thread-safety: each GraphRun has its own GraphRunner instance.
 * No shared mutable state between concurrent runs.
 */
class GraphRunner {
public:
    /**
     * @param store         GraphStore (for persisting run results)
     * @param graph         Full graph to execute
     * @param run_id       Current run ID
     * @param inputs       Graph input values (from POST /api/graphs/:id/run body)
     */
    GraphRunner(class GraphStore& store,
                class ExecutorRegistry& registry,
                const Graph& graph,
                const std::string& run_id,
                std::unordered_map<std::string, std::string> inputs);

    ~GraphRunner();

    // Non-copyable / movable
    GraphRunner(const GraphRunner&) = delete;
    GraphRunner& operator=(const GraphRunner&) = delete;

    /**
     * Run the graph synchronously (blocking).
     * Catches all exceptions and marks the run as failed.
     */
    void run_sync();

    /**
     * Run the graph as a jthread (cancellable).
     * Must be called from a std::jthread context.
     */
    void run(std::stop_token stop);

    /**
     * Cancel the running graph.
     */
    void cancel();

    // True once cancel() has been requested (takes effect between nodes).
    bool cancelled() const { return cancelled_.load(); }

    // True once a node executor has thrown; fail_error() has the message.
    bool failed() const { return failed_.load(); }
    const std::string& fail_error() const { return fail_error_; }

    /**
     * Callback for streaming output events.
     * Called from the execution thread — caller handles synchronisation.
     */
    using EventCallback = std::function<void(json event)>;
    void set_event_callback(EventCallback cb) { on_event_ = std::move(cb); }

private:
    void emit_event(const json& e);

    // Topological sort + ready-queue management
    void build_ready_queue();
    bool all_inputs_ready(const Node& node) const;
    std::string resolve_input(const Node& node, const std::string& port) const;

    // Execution
    void execute_ready_nodes();
    void execute_node(const Node& node);
    void record_node_failure(const Node& node, const std::string& error);
    bool handle_loop_end(const Node& node, const std::string& body_text);

    class GraphStore& store_;
    class ExecutorRegistry& registry_;
    const Graph& graph_;
    const std::string run_id_;

    // Resolved values: node_id + port → value
    // Guards resolved_ / completed_ / ready_ — worker threads execute nodes
    // in parallel and all three are shared mutable state.
    std::mutex state_mtx_;

    std::unordered_map<std::string /* node_id:port */, std::string> resolved_;
    // Which nodes have been completed in the current pass
    std::unordered_set<std::string> completed_;
    // Loop engine (tracks iteration counters + feedback text)
    LoopEngine loop_engine_;
    // Feedback text for loop_start nodes during a jump-back.
    std::unordered_map<std::string, std::string> loop_feedback_;
    // Pending nodes in the ready queue
    std::vector<std::string> ready_;
    // Max concurrent agent nodes
    int max_concurrent_ = 4;
    // Flag to stop execution
    std::atomic<bool> cancelled_{false};
    // Set when any node executor throws — per design.md §2.5 a node failure
    // fails the whole run (no retry, no silent re-dispatch).
    std::atomic<bool> failed_{false};
    std::string fail_error_;
    // Current iteration for each loop (node_id → iteration)
    std::unordered_map<std::string, int> loop_iterations_;
    // Event callback
    EventCallback on_event_;
};
