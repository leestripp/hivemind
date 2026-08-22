#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <optional>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

// ---------------------------------------------------------------------------
// Port
// ---------------------------------------------------------------------------
struct Port {
    std::string name;    // e.g. "in", "out"
    std::string type = "string"; // "string" | "json"
    std::string direction; // "input" | "output" — set by ports_for_type()
};

// ---------------------------------------------------------------------------
// Node — stored in SQLite, config_json holds type-specific fields
// ---------------------------------------------------------------------------
struct Node {
    std::string id;            // UUID v4
    std::string graph_id;      // FK to graphs.id
    std::string type;          // "prompt" | "agent" | "loop_start" | "loop_end" | "input" | "output" | "log"
    std::string label;         // User-facing label
    json config_json;          // Type-specific config (see CodeMax.md §6)
    double position_x = 0;
    double position_y = 0;
    std::vector<Port> inputs;  // Derived from type (not stored)
    std::vector<Port> outputs; // Derived from type (not stored)

    // Returns the standard port list for this node type
    static std::vector<Port> ports_for_type(const std::string& type);
};

// ---------------------------------------------------------------------------
// Edge — connects output port → input port
// ---------------------------------------------------------------------------
struct Edge {
    std::string id;           // UUID v4
    std::string graph_id;     // FK to graphs.id
    std::string source_node;  // Node ID
    std::string source_port;  // e.g. "out"
    std::string target_node;  // Node ID
    std::string target_port;  // e.g. "in"
};

// ---------------------------------------------------------------------------
// Graph metadata (no nodes/edges — those are loaded separately)
// ---------------------------------------------------------------------------
struct GraphMeta {
    std::string id;
    std::string name;
    std::string description;
    std::string created_at; // ISO-8601
    std::string updated_at;
};

// ---------------------------------------------------------------------------
// Full Graph with nodes + edges (used in API responses and execution)
// ---------------------------------------------------------------------------
struct Graph {
    GraphMeta meta;
    std::vector<Node> nodes;
    std::vector<Edge> edges;
};

// ---------------------------------------------------------------------------
// Execution result value
// ---------------------------------------------------------------------------
struct NodeResult {
    std::string node_id;
    std::string output_port;  // which output port produced the value
    std::string value;        // serialised string value
};

// ---------------------------------------------------------------------------
// Graph run state
// ---------------------------------------------------------------------------
struct GraphRun {
    std::string run_id;       // UUID v4
    std::string graph_id;
    std::string status;       // "running" | "completed" | "failed" | "cancelled"
    std::string error;        // populated on failure
    std::unordered_map<std::string /* node_id */, std::string /* output_port -> value */> results;
    std::vector<std::string> completed_nodes;  // execution order
    std::chrono::steady_clock::time_point started_at;
    std::chrono::steady_clock::time_point finished_at;
};
