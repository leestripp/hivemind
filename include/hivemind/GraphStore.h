#pragma once

#include "hivemind/Graph.h"
#include <string>
#include <optional>
#include <filesystem>
#include <vector>
#include <sqlite3.h>

/**
 * GraphStore — SQLite persistence layer for graphs, nodes, and edges.
 *
 * Thread-safety: SQLite connections are not thread-safe.
 * Each operation opens a fresh connection, does its work, closes.
 * For the execution engine's parallel node reads, a separate
 * read-only connection per thread is acceptable.
 *
 * DB file: ~/.local/share/hivemind/hivemind.db (created on first access).
 */
// A registered trigger that can auto-fire a graph run.
// v1 supports kind="webhook" only (fired via POST /api/hooks/:gid/:secret).
struct Trigger {
    std::string id;
    std::string graph_id;
    std::string kind;          // "webhook" | "schedule"
    std::string secret;        // URL path secret (webhook kind)
    std::string config_json;   // {"schedule":{...}, "inputs":{...}} (schedule kind)
    std::string next_fire_at;  // ISO local; "" = not scheduled
    bool        enabled = true;
    std::string created_at;
    std::string last_fired_at; // "" if never
};

class GraphStore {
public:
    explicit GraphStore();
    ~GraphStore();

    // ------------------------------------------------------------------------
    // Graphs
    // ------------------------------------------------------------------------

    /// Create a new graph, return its ID.
    std::string create_graph(const std::string& name,
                             const std::string& description = "");

    /// Load graph metadata (no nodes/edges).
    std::optional<GraphMeta> get_graph_meta(const std::string& id);

    /// Load full graph (nodes + edges).
    std::optional<Graph> get_graph(const std::string& id);

    /// Update graph name/description.
    void update_graph(const std::string& id,
                      const std::string& name,
                      const std::string& description);

    /// Delete graph + all nodes + edges.
    void delete_graph(const std::string& id);

    /// List all graph metadata.
    std::vector<GraphMeta> list_graphs();

    // ------------------------------------------------------------------------
    // Nodes
    // ------------------------------------------------------------------------

    /// Add a node to a graph. Returns the new node ID.
    std::string add_node(const std::string& graph_id,
                         const std::string& type,
                         const std::string& label,
                         const json& config_json,
                         double pos_x,
                         double pos_y);

    /// Update node label / config / position.
    void update_node(const std::string& node_id,
                     const std::string& label,
                     const json& config_json,
                     double pos_x,
                     double pos_y);

    /// Delete a node and all its connected edges.
    void delete_node(const std::string& node_id);

    /// Load all nodes for a graph.
    std::vector<Node> get_nodes(const std::string& graph_id);

    // ------------------------------------------------------------------------
    // Edges
    // ------------------------------------------------------------------------

    /// Add an edge. Returns empty string on duplicate/target-not-found.
    std::string add_edge(const std::string& graph_id,
                          const std::string& source_node,
                          const std::string& source_port,
                          const std::string& target_node,
                          const std::string& target_port);

    /// Delete an edge.
    void delete_edge(const std::string& edge_id);

    /// Load all edges for a graph.
    std::vector<Edge> get_edges(const std::string& graph_id);

    // ------------------------------------------------------------------------
    // Runs
    // ------------------------------------------------------------------------

    /// Create a new run record.
    std::string create_run(const std::string& graph_id);

    /// Update run status / error.
    void update_run(const std::string& run_id,
                    const std::string& status,
                    const std::string& error = "");

    /// Upsert a node result into the run's result map.
    void set_run_result(const std::string& run_id,
                        const std::string& node_id,
                        const std::string& port,
                        const std::string& value);

    /// Upsert a completed node entry.
    void add_run_completed(const std::string& run_id, const std::string& node_id);

    /// Load full run state.
    std::optional<GraphRun> get_run(const std::string& run_id);

    // ── Triggers ───────────────────────────────────────────────────────
    std::string create_trigger(const std::string& graph_id, const std::string& kind,
                               const std::string& config_json = "",
                               const std::string& next_fire_at = "");
    std::vector<Trigger> list_triggers(const std::string& graph_id);
    std::vector<Trigger> list_all_triggers();
    /// Enabled schedule triggers whose next_fire_at <= now (ISO local).
    std::vector<Trigger> list_due_schedules(const std::string& now_iso);
    /// Record a firing: set last_fired_at, next_fire_at, optionally disable.
    void mark_trigger_fired(const std::string& id, const std::string& fired_at,
                            const std::string& next_fire_at, bool disable);
    void set_trigger_enabled(const std::string& id, bool enabled);
    std::optional<Trigger> get_trigger(const std::string& graph_id,
                                       const std::string& secret);
    void delete_trigger(const std::string& id);
    void touch_trigger(const std::string& id);   // set last_fired_at = now

private:
    std::filesystem::path db_path_;
    void ensure_schema();

    // Helper: open a new sqlite3 connection
    sqlite3* open_conn();
};
