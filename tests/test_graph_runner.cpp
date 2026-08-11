// test_graph_runner.cpp — end-to-end GraphRunner tests (no server, no LLM).
//
// Builds graphs in memory and runs them through GraphRunner::run_sync(),
// asserting on the emitted event stream and the persisted run results.
// The DB is isolated by pointing HOME at a temp dir before GraphStore
// construction (GraphStore derives its path from $HOME).

#include <cassert>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <string>
#include <vector>

#include "hivemind/ExecutorRegistry.h"
#include "hivemind/Graph.h"
#include "hivemind/GraphRunner.h"
#include "hivemind/GraphStore.h"

namespace fs = std::filesystem;

namespace {

Node mk_node(const std::string& id, const std::string& type,
             const std::string& label, json config = json::object()) {
    Node n;
    n.id = id; n.type = type; n.label = label;
    n.config_json = std::move(config);
    n.inputs  = Node::ports_for_type(type) ;
    n.outputs = n.inputs;  // ports_for_type returns both; filtered below
    // ports_for_type returns input+output combined; split by direction:
    std::vector<Port> in, out;
    for (auto& p : n.inputs) (p.direction == "input" ? in : out).push_back(p);
    n.inputs = std::move(in); n.outputs = std::move(out);
    return n;
}

Edge mk_edge(const std::string& id, const std::string& sn, const std::string& sp,
             const std::string& tn, const std::string& tp) {
    Edge e;
    e.id = id; e.source_node = sn; e.source_port = sp;
    e.target_node = tn; e.target_port = tp;
    return e;
}

// Collects events from worker threads safely.
struct EventLog {
    mutable std::mutex m;
    std::vector<json> events;
    void push(json e) { std::lock_guard lk{m}; events.push_back(std::move(e)); }
    int count(const std::string& kind, const std::string& node_id = "") const {
        std::lock_guard lk{m};
        int n = 0;
        for (auto& e : events)
            if (e.value("event", "") == kind
                && (node_id.empty() || e.value("node_id", "") == node_id)) ++n;
        return n;
    }
};

// Runs a graph; returns results map. Fails the test via assert on failure.
std::unordered_map<std::string, std::string>
run_graph(GraphStore& store, ExecutorRegistry& reg, Graph g,
          const std::string& gid,
          std::unordered_map<std::string, std::string> inputs,
          EventLog& log)
{
    std::string run_id = store.create_run(gid);
    GraphRunner runner{store, reg, g, run_id,
                       std::unordered_map<std::string, std::string>{std::move(inputs)}};
    runner.set_event_callback([&log](json e) { log.push(std::move(e)); });
    runner.run_sync();
    if (runner.failed()) {
        std::cerr << "run failed: " << runner.fail_error() << "\n";
        assert(false && "run failed");
    }
    auto run = store.get_run(run_id);
    assert(run.has_value());
    return run->results;
}

} // namespace

int main() {
    // ── Isolate the DB ──────────────────────────────────────────────────
    char tmpl[] = "/tmp/hivemind-runner-test-XXXXXX";
    const char* dir = mkdtemp(tmpl);
    assert(dir);
    setenv("HOME", dir, 1);

    GraphStore store;
    ExecutorRegistry registry;
    std::string gid = store.create_graph("runner-e2e", "test graph");

    // ── Test 1: input + prompt → merge → output ─────────────────────────
    {
        Graph g; g.meta.id = gid; g.meta.name = "merge-flow";
        g.nodes = {
            mk_node("i1", "input",  "topic",   {{"param_name", "topic"}}),
            mk_node("p1", "prompt", "static",  {{"value", "PROMPT-TEXT"}}),
            mk_node("m1", "merge",  "join"),
            mk_node("o1", "output", "result",  {{"output_name", "result"}}),
        };
        g.edges = {
            mk_edge("e1", "i1", "out", "m1", "in_a"),
            mk_edge("e2", "p1", "out", "m1", "in_b"),
            mk_edge("e3", "m1", "out", "o1", "in"),
        };
        EventLog log;
        auto results = run_graph(store, registry, g, gid, {{"topic", "RUNTIME-INPUT"}}, log);

        assert(log.count("graph_started") == 1);
        assert(log.count("node_completed", "i1") == 1);
        assert(log.count("node_completed", "p1") == 1);
        assert(log.count("node_completed", "m1") == 1);
        assert(log.count("node_completed", "o1") == 1);
        assert(log.count("graph_completed") == 1);

        auto it = results.find("result");
        assert(it != results.end() && "output_name key missing from results");
        assert(it->second.find("RUNTIME-INPUT") != std::string::npos);
        assert(it->second.find("PROMPT-TEXT")  != std::string::npos);
        std::cout << "✓ test 1: input+prompt -> merge -> output\n";
    }

    // ── Test 2: loop_start → prompt → loop_end (x3) → output ────────────
    {
        Graph g; g.meta.id = gid; g.meta.name = "loop-flow";
        g.nodes = {
            mk_node("ls", "loop_start", "start", {{"loop_count", 3}}),
            mk_node("bp", "prompt",     "body",  {{"value", "BODY"}}),
            mk_node("le", "loop_end",   "end"),
            mk_node("o1", "output",     "result", {{"output_name", "result"}}),
        };
        g.edges = {
            mk_edge("e1", "ls", "out",        "bp", "in"),
            mk_edge("e2", "bp", "out",        "le", "in"),
            mk_edge("e3", "ls", "loop_count", "le", "loop_count"),
            mk_edge("e4", "le", "out",        "o1", "in"),
        };
        EventLog log;
        auto results = run_graph(store, registry, g, gid, {}, log);

        // Body must execute exactly 3 times; loop_start re-fires 3 times total.
        assert(log.count("node_started", "bp") == 3);
        assert(log.count("loop_iteration") == 2);          // jumps back twice
        assert(log.count("node_completed", "le") == 1);    // completes only on final iteration
        assert(log.count("graph_completed") == 1);
        auto it = results.find("result");
        assert(it != results.end() && it->second == "BODY");
        std::cout << "✓ test 2: loop body ran exactly 3 times\n";
    }

    // ── Test 3: parallel fan-out — two prompts race into one merge ───────
    {
        Graph g; g.meta.id = gid; g.meta.name = "fanout";
        g.nodes = {
            mk_node("p1", "prompt", "A", {{"value", "AAA"}}),
            mk_node("p2", "prompt", "B", {{"value", "BBB"}}),
            mk_node("m1", "merge",  "join"),
            mk_node("o1", "output", "result", {{"output_name", "result"}}),
        };
        g.edges = {
            mk_edge("e1", "p1", "out", "m1", "in_a"),
            mk_edge("e2", "p2", "out", "m1", "in_b"),
            mk_edge("e3", "m1", "out", "o1", "in"),
        };
        EventLog log;
        auto results = run_graph(store, registry, g, gid, {}, log);

        assert(log.count("node_completed", "p1") == 1);
        assert(log.count("node_completed", "p2") == 1);
        assert(log.count("graph_completed") == 1);
        const std::string& v = results.at("result");
        assert(v.find("AAA") != std::string::npos && v.find("BBB") != std::string::npos);
        std::cout << "✓ test 3: parallel fan-out merged both branches\n";
    }

    // ── Test 4: agent_loop_start registered with LoopEngine (regression) ──
    // Regression test: agent_loop_start used to be skipped by the
    // GraphRunner's LoopEngine registration, so loop_end_reached() always
    // returned false and the loop exited after one iteration. Pair an
    // agent_loop_start with a PLAIN loop_end (no judge agent → no LLM
    // needed) — the loop machinery must still honor max_iterations.
    {
        Graph g; g.meta.id = gid; g.meta.name = "agent-loop-machinery";
        g.nodes = {
            mk_node("als", "agent_loop_start", "start", {{"max_iterations", 3}}),
            mk_node("bp",  "prompt",           "body",  {{"value", "BODY"}}),
            mk_node("le",  "loop_end",         "end"),
            mk_node("o1",  "output",           "result", {{"output_name", "result"}}),
        };
        g.edges = {
            mk_edge("e1", "als", "out",        "bp", "in"),
            mk_edge("e2", "bp",  "out",        "le", "in"),
            mk_edge("e3", "als", "loop_count", "le", "loop_count"),
            mk_edge("e4", "le",  "out",        "o1", "in"),
        };
        EventLog log;
        auto results = run_graph(store, registry, g, gid, {}, log);

        assert(log.count("node_started", "bp") == 3);   // body ran 3 times
        assert(log.count("loop_iteration") == 2);       // jumped back twice
        assert(log.count("node_completed", "le") == 1); // completes on final iter
        assert(log.count("graph_completed") == 1);
        auto it = results.find("result");
        assert(it != results.end() && it->second == "BODY");
        std::cout << "✓ test 4: agent_loop_start honors max_iterations\n";
    }

    fs::remove_all(dir);
    std::cout << "GraphRunner end-to-end tests passed\n";
    return 0;
}
