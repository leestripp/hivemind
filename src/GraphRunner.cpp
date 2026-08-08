#include "hivemind/GraphRunner.h"
#include "hivemind/GraphStore.h"
#include "hivemind/ExecutorRegistry.h"
#include "hivemind/NodeExecutor.h"
#include <algorithm>
#include <iostream>
#include <sstream>
#include <unordered_set>

namespace {

// Topological sort (Kahn's algorithm) — returns nodes in execution order.
// Throws if a cycle is found (outside of explicit loop nodes).
std::vector<std::string> topo_sort(const std::vector<Node>& nodes,
                                    const std::vector<Edge>& edges) {
    std::unordered_map<std::string, int> in_degree;
    for (auto& n : nodes) in_degree[n.id] = 0;
    std::unordered_map<std::string, std::vector<std::string>> adj;
    for (auto& e : edges) {
        adj[e.source_node].push_back(e.target_node);
        in_degree[e.target_node]++;
    }
    std::queue<std::string> q;
    for (auto& [id, deg] : in_degree) if (deg == 0) q.push(id);
    std::vector<std::string> order;
    while (!q.empty()) {
        std::string cur = q.front(); q.pop();
        order.push_back(cur);
        for (auto& nxt : adj[cur]) {
            if (--in_degree[nxt] == 0) q.push(nxt);
        }
    }
    if (order.size() != nodes.size())
        throw std::runtime_error("graph contains a cycle");
    return order;
}

const Node* find_node(const std::vector<Node>& nodes, const std::string& id) {
    for (auto& n : nodes) if (n.id == id) return &n;
    return nullptr;
}

} // namespace

GraphRunner::GraphRunner(GraphStore& store,
                         ExecutorRegistry& registry,
                         const Graph& graph,
                         const std::string& run_id,
                         std::unordered_map<std::string, std::string> inputs)
    : store_(store), registry_(registry), graph_(graph), run_id_(run_id)
{
    // Build resolved_ map from graph inputs
    for (auto& n : graph_.nodes) {
        if (n.type == "input") {
            std::string param_name;
            if (n.config_json.contains("param_name") && n.config_json["param_name"].is_string())
                param_name = n.config_json["param_name"].get<std::string>();
            auto it = inputs.find(param_name);
            if (it != inputs.end()) {
                resolved_[n.id + ":out"] = it->second;
            }
        }
    }

    // Register loop nodes with the engine
    for (auto& n : graph_.nodes) {
        if (n.type == "loop_start") {
            int count = 1;
            if (n.config_json.contains("loop_count") && n.config_json["loop_count"].is_number_integer())
                count = n.config_json["loop_count"].get<int>();
            else if (n.config_json.contains("loop_count") && n.config_json["loop_count"].is_string())
                count = std::stoi(n.config_json["loop_count"].get<std::string>());
            loop_engine_.register_loop(n.id, count);
        }
    }
}

GraphRunner::~GraphRunner() = default;

void GraphRunner::emit_event(const json& e) {
    if (on_event_) on_event_(e);
}

void GraphRunner::run_sync() {
    // Blocking: run on the calling thread. The caller (RunRoutes) already
    // provides the background thread — spawning a local jthread here would
    // have its destructor request_stop() the moment run_sync() returned,
    // immediately cancelling the run.
    run(std::stop_token{});
}

void GraphRunner::run(std::stop_token st) {
    try {
        // Topological order
        auto order = topo_sort(graph_.nodes, graph_.edges);
        (void)order; // not used directly; we manage ready queue

        // Initial ready queue: all nodes with no incoming edges
        for (auto& n : graph_.nodes) {
            bool has_incoming = false;
            for (auto& e : graph_.edges)
                if (e.target_node == n.id) { has_incoming = true; break; }
            if (!has_incoming) ready_.push_back(n.id);
        }

        emit_event({{"event", "graph_started"}, {"graph_id", graph_.meta.id}, {"run_id", run_id_}});

        while (!ready_.empty() && !st.stop_requested() && !cancelled_ && !failed_) {
            // Execute ready nodes in parallel
            execute_ready_nodes();
            if (cancelled_ || failed_) break;

            // If loop end triggered a jump-back, push loop_start back onto ready
            // (handled inside execute_ready_nodes via loop_engine_)
            if (ready_.empty() && !cancelled_ && !failed_) {
                emit_event({{"event", "graph_completed"}});
                store_.update_run(run_id_, "completed");
            }
        }

        if (failed_) {
            emit_event({{"event", "graph_failed"}, {"error", fail_error_}});
            store_.update_run(run_id_, "failed", fail_error_);
        }

        if (st.stop_requested() || cancelled_) {
            emit_event({{"event", "graph_cancelled"}});
            store_.update_run(run_id_, "cancelled");
        }
    } catch (const std::exception& e) {
        std::cerr << "[GraphRunner] fatal: " << e.what() << "\n";
        emit_event({{"event", "graph_failed"}, {"error", e.what()}});
        store_.update_run(run_id_, "failed", e.what());
    }
}

void GraphRunner::cancel() {
    cancelled_ = true;
}

bool GraphRunner::all_inputs_ready(const Node& node) const {
    for (auto& e : graph_.edges) {
        if (e.target_node == node.id) {
            std::string key = e.source_node + ":" + e.source_port;
            if (resolved_.find(key) == resolved_.end())
                return false;
        }
    }
    return true;
}

std::string GraphRunner::resolve_input(const Node& node, const std::string& port) const {
    // Find the edge feeding this port
    for (auto& e : graph_.edges) {
        if (e.target_node == node.id && e.target_port == port) {
            std::string key = e.source_node + ":" + e.source_port;
            auto it = resolved_.find(key);
            if (it != resolved_.end()) return it->second;
        }
    }
    return "";
}

void GraphRunner::execute_ready_nodes() {
    // Group by whether they're the start of a loop (sequential) or parallel
    std::vector<std::string> sequential;
    std::vector<std::string> parallel;

    for (auto& nid : ready_) {
        auto* n = find_node(graph_.nodes, nid);
        if (!n) continue;
        if (n->type == "loop_start" || n->type == "loop_end") {
            sequential.push_back(nid);
        } else {
            parallel.push_back(nid);
        }
    }

    // Execute parallel nodes with std::async
    std::vector<std::future<void>> futures;
    for (auto& nid : parallel) {
        auto* n = find_node(graph_.nodes, nid);
        if (!n) continue;
        futures.push_back(std::async(std::launch::async, [this, n]() {
            try { execute_node(*n); }
            catch (const std::exception& e) { record_node_failure(*n, e.what()); }
        }));
    }
    for (auto& f : futures) f.wait();

    // Execute loop nodes sequentially (order matters)
    for (auto& nid : sequential) {
        auto* n = find_node(graph_.nodes, nid);
        if (!n) continue;
        try { execute_node(*n); }
        catch (const std::exception& e) { record_node_failure(*n, e.what()); }
    }

    // Rebuild ready queue (lock: workers mutated completed_ / resolved_)
    {
        std::lock_guard<std::mutex> lk{state_mtx_};
        ready_.clear();
        for (auto& n : graph_.nodes) {
            if (completed_.count(n.id)) continue;
            if (all_inputs_ready(n)) ready_.push_back(n.id);
        }
    }
}

void GraphRunner::record_node_failure(const Node& node, const std::string& error) {
    std::cerr << "[GraphRunner] node " << node.id << " failed: " << error << "\n";
    emit_event({{"event", "node_failed"},
                {"node_id", node.id},
                {"node_label", node.label},
                {"error", error}});
    {
        std::lock_guard<std::mutex> lk{state_mtx_};
        completed_.insert(node.id);   // never re-dispatch a failed node
        if (!failed_.load()) {
            failed_.store(true);
            fail_error_ = "node '" + node.label + "' (" + node.type + "): " + error;
        }
    }
}

void GraphRunner::execute_node(const Node& node) {
    emit_event({{"event", "node_started"}, {"node_id", node.id}, {"node_label", node.label}});

    // Gather inputs (lock: other workers write resolved_ concurrently)
    std::unordered_map<std::string, std::string> inputs;
    {
        std::lock_guard<std::mutex> lk{state_mtx_};
        for (auto& e : graph_.edges) {
            if (e.target_node == node.id) {
                std::string key = e.source_node + ":" + e.source_port;
                auto it = resolved_.find(key);
                if (it != resolved_.end())
                    inputs[e.target_port] = it->second;
            }
        }
        // input nodes: surface the value seeded from the run payload so the
        // executor can pass it through instead of overwriting it with "".
        if (node.type == "input") {
            auto it = resolved_.find(node.id + ":out");
            if (it != resolved_.end()) inputs["out"] = it->second;
        }

        // loop_start: on the first iteration use the upstream edge input; on
        // subsequent iterations the feedback text from the previous loop_end
        // overwrites it. The feedback is stored in loop_feedback_ by
        // handle_loop_end() when it jumps back.
        if (node.type == "loop_start") {
            auto it = loop_feedback_.find(node.id);
            if (it != loop_feedback_.end()) {
                inputs["in"] = it->second;
            }
        }
    }

    // Execute
    NodeExecutor* exec = registry_.get(node.type);
    if (!exec) throw std::runtime_error("unknown node type: " + node.type);

    auto results = exec->execute(node, inputs, run_id_);

    // Handle loop_end specifically
    if (node.type == "loop_end") {
        std::string body_text = inputs.count("in") ? inputs.at("in") : std::string{};
        bool jump_back = handle_loop_end(node, body_text);
        if (!jump_back) {
            // Final iteration: emit loop_end's out port and mark it completed.
            std::lock_guard<std::mutex> lk{state_mtx_};
            resolved_[node.id + ":out"] = body_text;
            store_.set_run_result(run_id_, node.id, "out", body_text);
            emit_event({{"event", "node_output"}, {"node_id", node.id}, {"chunk", body_text}});
            completed_.insert(node.id);
        }
        // If jump_back is true, leave loop_end uncompleted in memory so the
        // ready queue can re-add it on the next iteration.
        store_.add_run_completed(run_id_, node.id);
        return;
    }

    // Handle agent_loop_end: the judge agent decides continue/exit.
    if (node.type == "agent_loop_end") {
        std::string cont_feedback, final_text;
        bool wants_continue = false;
        for (auto& r : results) {
            if (r.output_port == "__continue__") { wants_continue = true; cont_feedback = r.value; }
            else if (r.output_port == "out") final_text = r.value;
        }
        bool jumped = false;
        if (wants_continue) {
            // Reuse the basic loop machinery (cap enforced via loop_count).
            jumped = handle_loop_end(node, cont_feedback);
        }
        if (!jumped) {
            std::lock_guard<std::mutex> lk{state_mtx_};
            resolved_[node.id + ":out"] = final_text;
            store_.set_run_result(run_id_, node.id, "out", final_text);
            emit_event({{"event", "node_output"}, {"node_id", node.id}, {"chunk", final_text}});
            completed_.insert(node.id);
        }
        store_.add_run_completed(run_id_, node.id);
        return;
    }

    // Record results (lock: resolved_ / completed_ are shared with other workers)
    {
        std::lock_guard<std::mutex> lk{state_mtx_};
        for (auto& r : results) {
            std::string key = r.node_id + ":" + r.output_port;
            resolved_[key] = r.value;
            store_.set_run_result(run_id_, r.node_id, r.output_port, r.value);
            emit_event({{"event", "node_output"}, {"node_id", node.id}, {"chunk", r.value}});
        }

        emit_event({{"event", "node_completed"}, {"node_id", node.id}});
        completed_.insert(node.id);
    }
    store_.add_run_completed(run_id_, node.id);

    // Check if any downstream nodes are now ready
    // (next execute_ready_nodes() will pick them up)
}

namespace {

std::unordered_set<std::string> descendants_of(const std::vector<Edge>& edges,
                                               const std::string& from) {
    std::unordered_set<std::string> out;
    std::queue<std::string> q;
    q.push(from);
    while (!q.empty()) {
        auto cur = q.front(); q.pop();
        for (auto& e : edges) {
            if (e.source_node == cur && !out.count(e.target_node)) {
                out.insert(e.target_node);
                q.push(e.target_node);
            }
        }
    }
    return out;
}

std::unordered_set<std::string> ancestors_of(const std::vector<Edge>& edges,
                                             const std::string& to) {
    std::unordered_set<std::string> out;
    std::queue<std::string> q;
    q.push(to);
    while (!q.empty()) {
        auto cur = q.front(); q.pop();
        for (auto& e : edges) {
            if (e.target_node == cur && !out.count(e.source_node)) {
                out.insert(e.source_node);
                q.push(e.source_node);
            }
        }
    }
    return out;
}

} // namespace

bool GraphRunner::handle_loop_end(const Node& node, const std::string& body_text) {
    // Find the paired loop_start via the implicit loop_count edge.
    std::string start_id;
    for (auto& e : graph_.edges) {
        if (e.target_node == node.id && e.target_port == "loop_count") {
            start_id = e.source_node;
            break;
        }
    }
    if (start_id.empty()) {
        // Fallback: use the first loop_start in the graph (no nested loops).
        for (auto& n : graph_.nodes) {
            if (n.type == "loop_start") { start_id = n.id; break; }
        }
    }
    if (start_id.empty()) return false;

    bool jump_back = loop_engine_.loop_end_reached(start_id, body_text);

    if (jump_back) {
        // Identify body nodes: descendants of start that can reach end.
        auto desc = descendants_of(graph_.edges, start_id);
        auto anc = ancestors_of(graph_.edges, node.id);
        std::unordered_set<std::string> body_nodes;
        for (auto& n : graph_.nodes) {
            if (n.id == start_id || n.id == node.id) continue;
            if (desc.count(n.id) && anc.count(n.id)) body_nodes.insert(n.id);
        }

        std::lock_guard<std::mutex> lk{state_mtx_};
        // Remove stale outputs so body nodes don't run on old data.
        resolved_.erase(start_id + ":out");
        resolved_.erase(start_id + ":loop_count");
        // Clear completed status for start, body, and end so they can re-run.
        completed_.erase(start_id);
        for (auto& bid : body_nodes) completed_.erase(bid);
        completed_.erase(node.id);
        // Feed the body text back into loop_start:in for the next iteration.
        loop_feedback_[start_id] = body_text;
        ready_.push_back(start_id);
        emit_event({{"event", "loop_iteration"},
                    {"node_id", start_id},
                    {"iteration", loop_engine_.current_iteration(start_id)}});
    }

    return jump_back;
}
