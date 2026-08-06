#include "hivemind/Graph.h"

std::vector<Port> Node::ports_for_type(const std::string& type) {
    if (type == "prompt") {
        return {{"out",    "string", "output"}};
    }
    if (type == "agent") {
        return {{"in",     "string", "input"},
                {"out",    "string", "output"}};
    }
    if (type == "loop_start") {
        return {{"in",         "string", "input"},
                {"out",        "string", "output"},
                {"loop_count", "string", "output"}};
    }
    if (type == "loop_end") {
        return {{"in",         "string", "input"},
                {"loop_count", "string", "input"},
                {"out",        "string", "output"}};
    }
    if (type == "input") {
        return {{"out",    "string", "output"}};
    }
    if (type == "output") {
        return {{"in",     "string", "input"}};
    }
    if (type == "log") {
        return {{"in",     "string", "input"}};
    }
    if (type == "llm") {
        return {{"in",     "string", "input"},
                {"out",    "string", "output"}};
    }
    if (type == "merge") {
        return {{"in_a",   "string", "input"},
                {"in_b",   "string", "input"},
                {"in_c",   "string", "input"},
                {"out",    "string", "output"}};
    }
    return {};
}
