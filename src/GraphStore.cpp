#include "hivemind/GraphStore.h"
#include "hivemind/Graph.h"
#include <sqlite3.h>
#include <filesystem>
#include <cstdio>
#include <cstring>
#include <chrono>
#include <stdexcept>
#include <sstream>
#include <optional>

namespace fs = std::filesystem;
using namespace std::chrono_literals;

namespace {

std::string uuid_v4() {
    std::array<unsigned char, 16> buf{};
    FILE* f = std::fopen("/dev/urandom", "rb");
    if (f) {
        std::fread(buf.data(), 1, 16, f);
        std::fclose(f);
    }
    char out[37];
    std::snprintf(out, sizeof(out),
        "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
        buf[0],buf[1],buf[2],buf[3],
        buf[4],buf[5],
        (buf[6] & 0x0f) | 0x40, buf[7],
        (buf[8] & 0x3f) | 0x80, buf[9], buf[10], buf[11],
        buf[12],buf[13],buf[14],buf[15]);
    return out;
}

std::string iso_now() {
    auto now = std::chrono::system_clock::now();
    auto time_t = std::chrono::system_clock::to_time_t(now);
    char buf[64];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", std::gmtime(&time_t));
    return buf;
}

// Bind a std::string as TEXT — old 5-arg sqlite3_bind_text API
inline void bind_text(sqlite3_stmt* stmt, int idx, const std::string& val) {
    sqlite3_bind_text(stmt, idx, val.c_str(), -1, SQLITE_TRANSIENT);
}

// Execute a statement that doesn't return rows (INSERT/UPDATE/DELETE)
inline bool exec_sql(sqlite3* db, const char* sql) {
    char* err = nullptr;
    int rc = sqlite3_exec(db, sql, nullptr, nullptr, &err);
    if (rc != SQLITE_OK) {
        std::string msg = err ? err : "unknown";
        sqlite3_free(err);
        throw std::runtime_error("exec failed: " + msg);
    }
    return true;
}

} // namespace

GraphStore::GraphStore() {
    auto dir = fs::path{std::getenv("HOME") ? std::getenv("HOME") : "."}
             / ".local" / "share" / "hivemind";
    fs::create_directories(dir);
    db_path_ = dir / "hivemind.db";
    ensure_schema();
}

GraphStore::~GraphStore() = default;

sqlite3* GraphStore::open_conn() {
    sqlite3* db = nullptr;
    int rc = sqlite3_open(db_path_.string().c_str(), &db);
    if (rc != SQLITE_OK) throw std::runtime_error("cannot open db");

    // Parallel node execution means several connections write concurrently.
    // Without a busy timeout the loser of the race gets SQLITE_BUSY and the
    // store method throws (this caused nodes to re-execute repeatedly).
    sqlite3_busy_timeout(db, 5000);

    // WAL lets readers proceed while a writer is active (get_run polling
    // while a run is in flight).
    sqlite3_exec(db, "PRAGMA journal_mode = WAL", nullptr, nullptr, nullptr);
    sqlite3_exec(db, "PRAGMA synchronous = NORMAL", nullptr, nullptr, nullptr);
    return db;
}

void GraphStore::ensure_schema() {
    auto* db = open_conn();
    const char* sql = R"(
        CREATE TABLE IF NOT EXISTS graphs (
            id          TEXT PRIMARY KEY,
            name        TEXT NOT NULL,
            description TEXT,
            created_at  TEXT NOT NULL,
            updated_at  TEXT NOT NULL
        );
        CREATE TABLE IF NOT EXISTS nodes (
            id          TEXT PRIMARY KEY,
            graph_id    TEXT NOT NULL,
            type        TEXT NOT NULL,
            label       TEXT NOT NULL,
            config_json TEXT NOT NULL DEFAULT '{}',
            position_x  REAL NOT NULL DEFAULT 0,
            position_y  REAL NOT NULL DEFAULT 0,
            FOREIGN KEY (graph_id) REFERENCES graphs(id) ON DELETE CASCADE
        );
        CREATE TABLE IF NOT EXISTS edges (
            id           TEXT PRIMARY KEY,
            graph_id     TEXT NOT NULL,
            source_node  TEXT NOT NULL,
            source_port  TEXT NOT NULL,
            target_node  TEXT NOT NULL,
            target_port  TEXT NOT NULL,
            FOREIGN KEY (graph_id) REFERENCES graphs(id) ON DELETE CASCADE,
            FOREIGN KEY (source_node) REFERENCES nodes(id) ON DELETE CASCADE,
            FOREIGN KEY (target_node) REFERENCES nodes(id) ON DELETE CASCADE
        );
        CREATE TABLE IF NOT EXISTS runs (
            id          TEXT PRIMARY KEY,
            graph_id    TEXT NOT NULL,
            status      TEXT NOT NULL DEFAULT 'running',
            error       TEXT,
            created_at  TEXT NOT NULL,
            finished_at TEXT,
            FOREIGN KEY (graph_id) REFERENCES graphs(id) ON DELETE CASCADE
        );
        CREATE TABLE IF NOT EXISTS run_results (
            run_id      TEXT NOT NULL,
            node_id     TEXT NOT NULL,
            port        TEXT NOT NULL,
            value       TEXT NOT NULL,
            PRIMARY KEY (run_id, node_id, port),
            FOREIGN KEY (run_id) REFERENCES runs(id) ON DELETE CASCADE
        );
        CREATE TABLE IF NOT EXISTS triggers (
            id            TEXT PRIMARY KEY,
            graph_id      TEXT NOT NULL REFERENCES graphs(id) ON DELETE CASCADE,
            kind          TEXT NOT NULL,
            secret        TEXT NOT NULL,
            enabled       INTEGER NOT NULL DEFAULT 1,
            created_at    TEXT NOT NULL,
            last_fired_at TEXT NOT NULL DEFAULT ''
        );

        CREATE TABLE IF NOT EXISTS run_completed (
            run_id      TEXT NOT NULL,
            node_id     TEXT NOT NULL,
            seq         INTEGER NOT NULL,
            PRIMARY KEY (run_id, node_id),
            FOREIGN KEY (run_id) REFERENCES runs(id) ON DELETE CASCADE
        );
    )";
    exec_sql(db, sql);
    sqlite3_close(db);
}

// ---------------------------------------------------------------------------
// Graphs
// ---------------------------------------------------------------------------

std::string GraphStore::create_graph(const std::string& name, const std::string& description) {
    auto* db = open_conn();
    std::string id = uuid_v4();
    std::string now = iso_now();
    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(db,
        "INSERT INTO graphs (id, name, description, created_at, updated_at) VALUES (?, ?, ?, ?, ?)",
        -1, &stmt, nullptr);
    bind_text(stmt, 1, id);
    bind_text(stmt, 2, name);
    bind_text(stmt, 3, description);
    bind_text(stmt, 4, now);
    bind_text(stmt, 5, now);
    if (sqlite3_step(stmt) != SQLITE_DONE) { sqlite3_finalize(stmt); sqlite3_close(db); throw std::runtime_error("insert graph failed"); }
    sqlite3_finalize(stmt);
    sqlite3_close(db);
    return id;
}

std::optional<GraphMeta> GraphStore::get_graph_meta(const std::string& id) {
    auto* db = open_conn();
    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(db, "SELECT id, name, description, created_at, updated_at FROM graphs WHERE id = ?", -1, &stmt, nullptr);
    bind_text(stmt, 1, id);
    if (sqlite3_step(stmt) != SQLITE_ROW) { sqlite3_finalize(stmt); sqlite3_close(db); return std::nullopt; }
    GraphMeta m;
    m.id = (char*)sqlite3_column_text(stmt, 0);
    m.name = (char*)sqlite3_column_text(stmt, 1);
    m.description = (char*)sqlite3_column_text(stmt, 2) ? (char*)sqlite3_column_text(stmt, 2) : "";
    m.created_at = (char*)sqlite3_column_text(stmt, 3);
    m.updated_at = (char*)sqlite3_column_text(stmt, 4);
    sqlite3_finalize(stmt);
    sqlite3_close(db);
    return m;
}

std::optional<Graph> GraphStore::get_graph(const std::string& id) {
    auto meta = get_graph_meta(id);
    if (!meta) return std::nullopt;
    Graph g;
    g.meta = *meta;
    g.nodes = get_nodes(id);
    g.edges = get_edges(id);
    return g;
}

void GraphStore::update_graph(const std::string& id, const std::string& name, const std::string& description) {
    auto* db = open_conn();
    std::string now = iso_now();
    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(db, "UPDATE graphs SET name = ?, description = ?, updated_at = ? WHERE id = ?", -1, &stmt, nullptr);
    bind_text(stmt, 1, name);
    bind_text(stmt, 2, description);
    bind_text(stmt, 3, now);
    bind_text(stmt, 4, id);
    if (sqlite3_step(stmt) != SQLITE_DONE) { sqlite3_finalize(stmt); sqlite3_close(db); throw std::runtime_error("update graph failed"); }
    sqlite3_finalize(stmt);
    sqlite3_close(db);
}

void GraphStore::delete_graph(const std::string& id) {
    auto* db = open_conn();
    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(db, "DELETE FROM graphs WHERE id = ?", -1, &stmt, nullptr);
    bind_text(stmt, 1, id);
    if (sqlite3_step(stmt) != SQLITE_DONE) { sqlite3_finalize(stmt); sqlite3_close(db); throw std::runtime_error("delete graph failed"); }
    sqlite3_finalize(stmt);
    sqlite3_close(db);
}

std::vector<GraphMeta> GraphStore::list_graphs() {
    auto* db = open_conn();
    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(db, "SELECT id, name, description, created_at, updated_at FROM graphs ORDER BY updated_at DESC", -1, &stmt, nullptr);
    std::vector<GraphMeta> out;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        GraphMeta m;
        m.id = (char*)sqlite3_column_text(stmt, 0);
        m.name = (char*)sqlite3_column_text(stmt, 1);
        m.description = (char*)sqlite3_column_text(stmt, 2) ? (char*)sqlite3_column_text(stmt, 2) : "";
        m.created_at = (char*)sqlite3_column_text(stmt, 3);
        m.updated_at = (char*)sqlite3_column_text(stmt, 4);
        out.push_back(m);
    }
    sqlite3_finalize(stmt);
    sqlite3_close(db);
    return out;
}

// ---------------------------------------------------------------------------
// Nodes
// ---------------------------------------------------------------------------

std::string GraphStore::add_node(const std::string& graph_id, const std::string& type,
                                  const std::string& label, const json& config_json,
                                  double pos_x, double pos_y) {
    auto* db = open_conn();
    std::string id = uuid_v4();
    std::string cfg = config_json.dump();
    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(db,
        "INSERT INTO nodes (id, graph_id, type, label, config_json, position_x, position_y) VALUES (?, ?, ?, ?, ?, ?, ?)",
        -1, &stmt, nullptr);
    bind_text(stmt, 1, id);
    bind_text(stmt, 2, graph_id);
    bind_text(stmt, 3, type);
    bind_text(stmt, 4, label);
    bind_text(stmt, 5, cfg);
    sqlite3_bind_double(stmt, 6, pos_x);
    sqlite3_bind_double(stmt, 7, pos_y);
    if (sqlite3_step(stmt) != SQLITE_DONE) { sqlite3_finalize(stmt); sqlite3_close(db); throw std::runtime_error("insert node failed"); }
    sqlite3_finalize(stmt);
    sqlite3_close(db);
    return id;
}

void GraphStore::update_node(const std::string& node_id, const std::string& label,
                             const json& config_json, double pos_x, double pos_y) {
    auto* db = open_conn();
    std::string cfg = config_json.dump();
    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(db,
        "UPDATE nodes SET label = ?, config_json = ?, position_x = ?, position_y = ? WHERE id = ?",
        -1, &stmt, nullptr);
    bind_text(stmt, 1, label);
    bind_text(stmt, 2, cfg);
    sqlite3_bind_double(stmt, 3, pos_x);
    sqlite3_bind_double(stmt, 4, pos_y);
    bind_text(stmt, 5, node_id);
    if (sqlite3_step(stmt) != SQLITE_DONE) { sqlite3_finalize(stmt); sqlite3_close(db); throw std::runtime_error("update node failed"); }
    sqlite3_finalize(stmt);
    sqlite3_close(db);
}

void GraphStore::delete_node(const std::string& node_id) {
    auto* db = open_conn();
    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(db, "DELETE FROM nodes WHERE id = ?", -1, &stmt, nullptr);
    bind_text(stmt, 1, node_id);
    if (sqlite3_step(stmt) != SQLITE_DONE) { sqlite3_finalize(stmt); sqlite3_close(db); throw std::runtime_error("delete node failed"); }
    sqlite3_finalize(stmt);
    sqlite3_close(db);
}

std::vector<Node> GraphStore::get_nodes(const std::string& graph_id) {
    auto* db = open_conn();
    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(db,
        "SELECT id, type, label, config_json, position_x, position_y FROM nodes WHERE graph_id = ?",
        -1, &stmt, nullptr);
    bind_text(stmt, 1, graph_id);
    std::vector<Node> out;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        Node n;
        n.id = (char*)sqlite3_column_text(stmt, 0);
        n.graph_id = graph_id;
        n.type = (char*)sqlite3_column_text(stmt, 1);
        n.label = (char*)sqlite3_column_text(stmt, 2);
        const char* cfg_raw = (char*)sqlite3_column_text(stmt, 3);
        if (cfg_raw) {
            try { n.config_json = json::parse(cfg_raw); }
            catch (...) { n.config_json = json::object(); }
        }
        n.position_x = sqlite3_column_double(stmt, 4);
        n.position_y = sqlite3_column_double(stmt, 5);
        n.inputs  = Node::ports_for_type(n.type); // all ports; caller uses [0] for in, [1] for out
        n.outputs = {}; // derived on demand
        out.push_back(n);
    }
    sqlite3_finalize(stmt);
    sqlite3_close(db);
    return out;
}

// ---------------------------------------------------------------------------
// Edges
// ---------------------------------------------------------------------------

std::string GraphStore::add_edge(const std::string& graph_id,
                                  const std::string& source_node, const std::string& source_port,
                                  const std::string& target_node, const std::string& target_port) {
    auto* db = open_conn();
    std::string id = uuid_v4();
    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(db,
        "INSERT INTO edges (id, graph_id, source_node, source_port, target_node, target_port) VALUES (?, ?, ?, ?, ?, ?)",
        -1, &stmt, nullptr);
    bind_text(stmt, 1, id);
    bind_text(stmt, 2, graph_id);
    bind_text(stmt, 3, source_node);
    bind_text(stmt, 4, source_port);
    bind_text(stmt, 5, target_node);
    bind_text(stmt, 6, target_port);
    if (sqlite3_step(stmt) != SQLITE_DONE) { sqlite3_finalize(stmt); sqlite3_close(db); return ""; }
    sqlite3_finalize(stmt);
    sqlite3_close(db);
    return id;
}

void GraphStore::delete_edge(const std::string& edge_id) {
    auto* db = open_conn();
    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(db, "DELETE FROM edges WHERE id = ?", -1, &stmt, nullptr);
    bind_text(stmt, 1, edge_id);
    if (sqlite3_step(stmt) != SQLITE_DONE) { sqlite3_finalize(stmt); sqlite3_close(db); throw std::runtime_error("delete edge failed"); }
    sqlite3_finalize(stmt);
    sqlite3_close(db);
}

std::vector<Edge> GraphStore::get_edges(const std::string& graph_id) {
    auto* db = open_conn();
    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(db,
        "SELECT id, source_node, source_port, target_node, target_port FROM edges WHERE graph_id = ?",
        -1, &stmt, nullptr);
    bind_text(stmt, 1, graph_id);
    std::vector<Edge> out;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        Edge e;
        e.id = (char*)sqlite3_column_text(stmt, 0);
        e.graph_id = graph_id;
        e.source_node = (char*)sqlite3_column_text(stmt, 1);
        e.source_port = (char*)sqlite3_column_text(stmt, 2);
        e.target_node = (char*)sqlite3_column_text(stmt, 3);
        e.target_port = (char*)sqlite3_column_text(stmt, 4);
        out.push_back(e);
    }
    sqlite3_finalize(stmt);
    sqlite3_close(db);
    return out;
}

// ---------------------------------------------------------------------------
// Runs
// ---------------------------------------------------------------------------

std::string GraphStore::create_run(const std::string& graph_id) {
    auto* db = open_conn();
    std::string id = uuid_v4();
    std::string now = iso_now();
    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(db,
        "INSERT INTO runs (id, graph_id, status, created_at) VALUES (?, ?, 'running', ?)",
        -1, &stmt, nullptr);
    bind_text(stmt, 1, id);
    bind_text(stmt, 2, graph_id);
    bind_text(stmt, 3, now);
    if (sqlite3_step(stmt) != SQLITE_DONE) { sqlite3_finalize(stmt); sqlite3_close(db); throw std::runtime_error("create run failed"); }
    sqlite3_finalize(stmt);
    sqlite3_close(db);
    return id;
}

void GraphStore::update_run(const std::string& run_id, const std::string& status, const std::string& error) {
    auto* db = open_conn();
    std::string now = iso_now();
    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(db,
        "UPDATE runs SET status = ?, error = ?, finished_at = ? WHERE id = ?",
        -1, &stmt, nullptr);
    bind_text(stmt, 1, status);
    bind_text(stmt, 2, error);
    bind_text(stmt, 3, now);
    bind_text(stmt, 4, run_id);
    if (sqlite3_step(stmt) != SQLITE_DONE) { sqlite3_finalize(stmt); sqlite3_close(db); throw std::runtime_error("update run failed"); }
    sqlite3_finalize(stmt);
    sqlite3_close(db);
}

void GraphStore::set_run_result(const std::string& run_id, const std::string& node_id,
                                  const std::string& port, const std::string& value) {
    auto* db = open_conn();
    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(db,
        "INSERT OR REPLACE INTO run_results (run_id, node_id, port, value) VALUES (?, ?, ?, ?)",
        -1, &stmt, nullptr);
    bind_text(stmt, 1, run_id);
    bind_text(stmt, 2, node_id);
    bind_text(stmt, 3, port);
    bind_text(stmt, 4, value);
    if (sqlite3_step(stmt) != SQLITE_DONE) { sqlite3_finalize(stmt); sqlite3_close(db); throw std::runtime_error("set run result failed"); }
    sqlite3_finalize(stmt);
    sqlite3_close(db);
}

void GraphStore::add_run_completed(const std::string& run_id, const std::string& node_id) {
    auto* db = open_conn();
    sqlite3_stmt* cnt = nullptr;
    sqlite3_prepare_v2(db, "SELECT COALESCE(MAX(seq), 0) + 1 FROM run_completed WHERE run_id = ?", -1, &cnt, nullptr);
    bind_text(cnt, 1, run_id);
    int seq = 1;
    if (sqlite3_step(cnt) == SQLITE_ROW) seq = (int)sqlite3_column_int64(cnt, 0);
    sqlite3_finalize(cnt);
    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(db,
        "INSERT OR REPLACE INTO run_completed (run_id, node_id, seq) VALUES (?, ?, ?)",
        -1, &stmt, nullptr);
    bind_text(stmt, 1, run_id);
    bind_text(stmt, 2, node_id);
    sqlite3_bind_int64(stmt, 3, seq);
    if (sqlite3_step(stmt) != SQLITE_DONE) { sqlite3_finalize(stmt); sqlite3_close(db); throw std::runtime_error("add run completed failed"); }
    sqlite3_finalize(stmt);
    sqlite3_close(db);
}

std::optional<GraphRun> GraphStore::get_run(const std::string& run_id) {
    auto* db = open_conn();
    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(db, "SELECT graph_id, status, error FROM runs WHERE id = ?", -1, &stmt, nullptr);
    bind_text(stmt, 1, run_id);
    if (sqlite3_step(stmt) != SQLITE_ROW) { sqlite3_finalize(stmt); sqlite3_close(db); return std::nullopt; }
    GraphRun r;
    r.run_id = run_id;
    r.graph_id = (char*)sqlite3_column_text(stmt, 0);
    r.status = (char*)sqlite3_column_text(stmt, 1);
    r.error = (char*)sqlite3_column_text(stmt, 2) ? (char*)sqlite3_column_text(stmt, 2) : "";
    sqlite3_finalize(stmt);

    sqlite3_prepare_v2(db, "SELECT node_id, port, value FROM run_results WHERE run_id = ?", -1, &stmt, nullptr);
    bind_text(stmt, 1, run_id);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        std::string nid = (char*)sqlite3_column_text(stmt, 0);
        std::string port = (char*)sqlite3_column_text(stmt, 1);
        std::string val = (char*)sqlite3_column_text(stmt, 2);
        // Output nodes store their public result key (output_name) in the
        // node_id column and leave port empty. Everything else keys as
        // "node_id:port".
        if (port.empty()) {
            r.results[nid] = val;
        } else {
            r.results[nid + ":" + port] = val;
        }
    }
    sqlite3_finalize(stmt);

    sqlite3_prepare_v2(db, "SELECT node_id FROM run_completed WHERE run_id = ? ORDER BY seq", -1, &stmt, nullptr);
    bind_text(stmt, 1, run_id);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        r.completed_nodes.push_back((char*)sqlite3_column_text(stmt, 0));
    }
    sqlite3_finalize(stmt);
    sqlite3_close(db);
    return r;
}

// ─── Triggers ────────────────────────────────────────────────────────────

std::string GraphStore::create_trigger(const std::string& graph_id,
                                       const std::string& kind) {
    std::string id = uuid_v4();
    std::string secret = uuid_v4();
    std::string now = iso_now();
    auto* db = open_conn();
    const char* sql =
        "INSERT INTO triggers (id, graph_id, kind, secret, enabled, created_at)"
        " VALUES (?,?,?,?,1,?)";
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db, sql, -1, &st, nullptr) != SQLITE_OK) {
        sqlite3_close(db);
        throw std::runtime_error("create_trigger prepare failed");
    }
    sqlite3_bind_text(st, 1, id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, graph_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 3, kind.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 4, secret.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 5, now.c_str(), -1, SQLITE_TRANSIENT);
    bool ok = sqlite3_step(st) == SQLITE_DONE;
    sqlite3_finalize(st);
    sqlite3_close(db);
    if (!ok) throw std::runtime_error("create_trigger insert failed");
    return id;
}

std::vector<Trigger> GraphStore::list_triggers(const std::string& graph_id) {
    std::vector<Trigger> out;
    auto* db = open_conn();
    const char* sql =
        "SELECT id, graph_id, kind, secret, enabled, created_at, last_fired_at"
        " FROM triggers WHERE graph_id = ? ORDER BY created_at";
    sqlite3_stmt* st = nullptr;
    sqlite3_prepare_v2(db, sql, -1, &st, nullptr);
    sqlite3_bind_text(st, 1, graph_id.c_str(), -1, SQLITE_TRANSIENT);
    while (sqlite3_step(st) == SQLITE_ROW) {
        Trigger t;
        t.id            = (const char*)sqlite3_column_text(st, 0);
        t.graph_id      = (const char*)sqlite3_column_text(st, 1);
        t.kind          = (const char*)sqlite3_column_text(st, 2);
        t.secret        = (const char*)sqlite3_column_text(st, 3);
        t.enabled       = sqlite3_column_int(st, 4) != 0;
        t.created_at    = (const char*)sqlite3_column_text(st, 5);
        t.last_fired_at = (const char*)sqlite3_column_text(st, 6);
        out.push_back(std::move(t));
    }
    sqlite3_finalize(st);
    sqlite3_close(db);
    return out;
}

std::optional<Trigger> GraphStore::get_trigger(const std::string& graph_id,
                                               const std::string& secret) {
    auto* db = open_conn();
    const char* sql =
        "SELECT id, graph_id, kind, secret, enabled, created_at, last_fired_at"
        " FROM triggers WHERE graph_id = ? AND secret = ?";
    sqlite3_stmt* st = nullptr;
    sqlite3_prepare_v2(db, sql, -1, &st, nullptr);
    sqlite3_bind_text(st, 1, graph_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, secret.c_str(), -1, SQLITE_TRANSIENT);
    std::optional<Trigger> out;
    if (sqlite3_step(st) == SQLITE_ROW) {
        Trigger t;
        t.id            = (const char*)sqlite3_column_text(st, 0);
        t.graph_id      = (const char*)sqlite3_column_text(st, 1);
        t.kind          = (const char*)sqlite3_column_text(st, 2);
        t.secret        = (const char*)sqlite3_column_text(st, 3);
        t.enabled       = sqlite3_column_int(st, 4) != 0;
        t.created_at    = (const char*)sqlite3_column_text(st, 5);
        t.last_fired_at = (const char*)sqlite3_column_text(st, 6);
        out = std::move(t);
    }
    sqlite3_finalize(st);
    sqlite3_close(db);
    return out;
}

void GraphStore::delete_trigger(const std::string& id) {
    auto* db = open_conn();
    sqlite3_stmt* st = nullptr;
    sqlite3_prepare_v2(db, "DELETE FROM triggers WHERE id = ?", -1, &st, nullptr);
    sqlite3_bind_text(st, 1, id.c_str(), -1, SQLITE_TRANSIENT);
    bool ok = sqlite3_step(st) == SQLITE_DONE;
    sqlite3_finalize(st);
    sqlite3_close(db);
    if (!ok) throw std::runtime_error("delete_trigger failed");
}

void GraphStore::touch_trigger(const std::string& id) {
    auto* db = open_conn();
    sqlite3_stmt* st = nullptr;
    sqlite3_prepare_v2(db, "UPDATE triggers SET last_fired_at = ? WHERE id = ?",
                       -1, &st, nullptr);
    std::string now = iso_now();
    sqlite3_bind_text(st, 1, now.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_step(st);
    sqlite3_finalize(st);
    sqlite3_close(db);
}
