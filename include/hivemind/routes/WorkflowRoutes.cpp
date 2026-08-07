// include/hivemind/routes/WorkflowRoutes.cpp
// -----------------------------------------------------------------------------
// Portable workflow files (.hivemind.json):
//   GET  /api/graphs/:id/export  → download versioned JSON
//   POST /api/graphs/import      → upload JSON → new graph with remapped ids
// [FILTERED_API_KEY]

#include "hivemind/GraphStore.h"

#include <OmniNet.h>
#include <nlohmann/json.hpp>

#include <chrono>
#include <ctime>
#include <unordered_map>

using json = nlohmann::json;

static std::string now_iso8601() {
    std::time_t t = std::time(nullptr);
    char buf[32];
    std::strftime(buf, sizeof buf, "%Y-%m-%dT%H:%M:%SZ", std::gmtime(&t));
    return buf;
}

void register_workflow_routes(OmniNet::HTTP::Server& server, GraphStore& store) {

    // ── Export ────────────────────────────────────────────────────────────
    server.get("/api/graphs/:id/export",
        [&store](const OmniNet::HTTP::Request& req, OmniNet::HTTP::Response& resp) {
            std::string id{req.pathParam(0)};
            auto g = store.get_graph(id);
            if (!g) { resp = OmniNet::HTTP::Response::notFound("graph not found"); return; }

            json nodes = json::array();
            for (auto& n : g->nodes) {
                nodes.push_back({
                    {"id", n.id}, {"type", n.type}, {"label", n.label},
                    {"config", n.config_json}, {"x", n.position_x}, {"y", n.position_y},
                });
            }
            json edges = json::array();
            for (auto& e : g->edges) {
                edges.push_back({
                    {"source", e.source_node}, {"source_port", e.source_port},
                    {"target", e.target_node}, {"target_port", e.target_port},
                });
            }
            // nlohmann::json orders object keys alphabetically → deterministic output
            json out{
                {"version", 1},
                {"name", g->meta.name},
                {"description", g->meta.description},
                {"saved_at", now_iso8601()},
                {"nodes", nodes},
                {"edges", edges},
            };
            resp = OmniNet::HTTP::Response::ok();
            resp.setContentType("application/json");
            resp.setHeader("Content-Disposition",
                           "attachment; filename=\"" + g->meta.name + ".hivemind.json\"");
            resp.setBody(out.dump(2));
        });

    // ── Import ────────────────────────────────────────────────────────────
    server.post("/api/graphs/import",
        [&store](const OmniNet::HTTP::Request& req, OmniNet::HTTP::Response& resp) {
            json body;
            try { body = json::parse(req.bodyAsString()); }
            catch (...) { resp = OmniNet::HTTP::Response::badRequest("invalid JSON"); return; }

            if (body.value("version", 0) != 1 || !body.contains("nodes") || !body.contains("edges")
                || !body["nodes"].is_array() || !body["edges"].is_array()) {
                resp = OmniNet::HTTP::Response::badRequest(
                    "not a v1 workflow file (need version, nodes[], edges[])");
                return;
            }

            // Validate node types up front.
            for (auto& n : body["nodes"]) {
                std::string type = n.value("type", "");
                if (Node::ports_for_type(type).empty()) {
                    resp = OmniNet::HTTP::Response::badRequest("unknown node type: " + type);
                    return;
                }
            }

            std::string name = body.value("name", "workflow");
            std::string gid;
            try {
                gid = store.create_graph(name + " (imported)", body.value("description", ""));
            } catch (const std::exception& e) {
                resp = OmniNet::HTTP::Response::badRequest(e.what()); return;
            }

            // Remap old ids → fresh ids; never overwrites an existing graph.
            std::unordered_map<std::string, std::string> id_map;
            try {
                for (auto& n : body["nodes"]) {
                    std::string old_id = n.value("id", "");
                    std::string new_id = store.add_node(gid,
                        n.value("type", ""), n.value("label", ""),
                        n.contains("config") && n["config"].is_object() ? n["config"] : json::object(),
                        n.value("x", 0.0), n.value("y", 0.0));
                    id_map[old_id] = new_id;
                }
                for (auto& e : body["edges"]) {
                    std::string src = id_map.count(e.value("source", "")) ? id_map[e.value("source", "")] : "";
                    std::string tgt = id_map.count(e.value("target", "")) ? id_map[e.value("target", "")] : "";
                    if (src.empty() || tgt.empty()) continue;  // dangling edge in file — skip
                    store.add_edge(gid, src, e.value("source_port", "out"),
                                        tgt, e.value("target_port", "in"));
                }
            } catch (const std::exception& e) {
                resp = OmniNet::HTTP::Response::badRequest(e.what()); return;
            }

            resp = OmniNet::HTTP::Response::created(gid);
            resp.setContentType("application/json");
            resp.setBody(json{{"graph_id", gid}, {"name", name + " (imported)"},
                              {"nodes", id_map.size()}}.dump());
        });
}
