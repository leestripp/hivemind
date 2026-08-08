#include <iostream>
#include <cassert>
#include "hivemind/Graph.h"

int main() {
    // Test port derivation for each node type
    auto p = Node::ports_for_type("prompt");
    assert(p.size() == 1);
    assert(p[0].name == "out");

    auto a = Node::ports_for_type("agent");
    assert(a.size() == 2);
    assert(a[0].name == "in");
    assert(a[1].name == "out");

    auto ls = Node::ports_for_type("loop_start");
    assert(ls.size() == 3);
    assert(ls[0].name == "in");
    assert(ls[1].name == "out");
    assert(ls[2].name == "loop_count");

    auto le = Node::ports_for_type("loop_end");
    assert(le.size() == 3);
    assert(le[0].name == "in");
    assert(le[1].name == "loop_count");
    assert(le[2].name == "out");

    auto inp = Node::ports_for_type("input");
    assert(inp.size() == 1);
    assert(inp[0].name == "out");

    auto out = Node::ports_for_type("output");
    assert(out.size() == 1);
    assert(out[0].name == "in");

    auto lg = Node::ports_for_type("log");
    assert(lg.size() == 1);
    assert(lg[0].name == "in");

    auto mg = Node::ports_for_type("merge");
    assert(mg.size() == 4);
    assert(mg[0].name == "in_a");
    assert(mg[1].name == "in_b");
    assert(mg[2].name == "in_c");
    assert(mg[3].name == "out");

    auto als = Node::ports_for_type("agent_loop_start");
    assert(als.size() == 3);
    assert(als[0].name == "in" && als[1].name == "out" && als[2].name == "loop_count");

    auto ale = Node::ports_for_type("agent_loop_end");
    assert(ale.size() == 4);
    assert(ale[0].name == "in" && ale[1].name == "prompt_in"
        && ale[2].name == "loop_count" && ale[3].name == "out");

    // Unknown type
    auto unk = Node::ports_for_type("unknown_node");
    assert(unk.size() == 0);

    std::cout << "test_node_ports: PASS\n";
    return 0;
}
