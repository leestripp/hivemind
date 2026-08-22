#include "hivemind/GraphStore.h"
#include "hivemind/Graph.h"
#include <OmniNet.h>
#include <nlohmann/json.hpp>
#include <iostream>
#include <vector>

using json = nlohmann::json;


// OmniNet v0.9.7 stores path params in an unordered_map keyed "0","1",…;
// indexed pathParam(i) iterates that map in hash order, so multi-param
// routes get their captures back in an unreliable order (observed: swapped).
// Extract positional segments from the raw path instead — deterministic.
static std::vector<std::string> path_segments(std::string_view path) {
    std::vector<std::string> out;
    size_t i = 0;
    while (i < path.size()) {
        size_t j = path.find('/', i);
        if (j == std::string_view::npos) j = path.size();
        if (j > i) out.emplace_back(path.substr(i, j - i));
        i = j + 1;
    }
    return out;
}

static std::optional<json> parse_body(std::string_view sv) {
    try { return json::parse(sv); }
    catch (...) { return std::nullopt; }
}

void register_graph_routes(OmniNet::HTTP::Server& server, GraphStore& store) {

    // GET /api/node-types — list registered executor types + port shapes.
    server.get("/api/node-types", [](const OmniNet::HTTP::Request& req,
                                     OmniNet::HTTP::Response& resp) {
        (void)req;
        static const std::vector<std::string> types{
            "input", "prompt", "agent",
            "merge", "loop_start", "loop_end", "agent_loop_start", "agent_loop_end", "output", "log"
        };
        json arr = json::array();
        for (auto& t : types) {
            json ports_in = json::array();
            json ports_out = json::array();
            for (auto& port : Node::ports_for_type(t)) {
                json p = json{{"name", port.name}, {"type", port.type}};
                if (port.direction == "input") ports_in.push_back(p);
                else                           ports_out.push_back(p);
            }
            arr.push_back(json{
                {"type", t},
                {"inputs",  ports_in},
                {"outputs", ports_out},
            });
        }
        OmniNet::HTTP::Response r = OmniNet::HTTP::Response::ok();
        r.setContentType("application/json");
        r.setBody(arr.dump());
        resp = r;
    });

    // GET /api/graphs
    server.get("/api/graphs", [&store](const OmniNet::HTTP::Request& req,
                                                  OmniNet::HTTP::Response& resp) {
        (void)req;
        auto graphs = store.list_graphs();
        json arr = json::array();
        for (auto& g : graphs) {
            arr.push_back(json{{"id", g.id}, {"name", g.name},
                               {"description", g.description},
                               {"created_at", g.created_at}, {"updated_at", g.updated_at}});
        }
        OmniNet::HTTP::Response r = OmniNet::HTTP::Response::ok();
        r.setContentType("application/json");
        r.setBody(arr.dump());
        resp = r;
    });

    // POST /api/graphs
    server.post("/api/graphs", [&store](const OmniNet::HTTP::Request& req,
                                                   OmniNet::HTTP::Response& resp) {
        auto body = parse_body(req.bodyAsString());
        if (!body) {
            resp = OmniNet::HTTP::Response::badRequest("invalid JSON body");
            return;
        }
        if (!body->contains("name") || !(*body)["name"].is_string()) {
            resp = OmniNet::HTTP::Response::badRequest("missing or invalid 'name' field");
            return;
        }
        std::string name = (*body)["name"].get<std::string>();
        std::string description;
        if (body->contains("description") && (*body)["description"].is_string())
            description = (*body)["description"].get<std::string>();
        try {
            std::string id = store.create_graph(name, description);
            OmniNet::HTTP::Response r = OmniNet::HTTP::Response::created(id);
            r.setContentType("application/json");
            r.setBody(json{{"id", id}}.dump());
            resp = r;
        } catch (const std::exception& e) {
            resp = OmniNet::HTTP::Response::badRequest(e.what());
        }
    });

    // GET /api/graphs/:id
    server.get("/api/graphs/:id", [&store](const OmniNet::HTTP::Request& req,
                                                     OmniNet::HTTP::Response& resp) {
        std::string id{req.pathParam(0)};
        auto g = store.get_graph(id);
        if (!g) { resp = OmniNet::HTTP::Response::notFound("graph not found"); return; }
        json j = json{{"id", g->meta.id}, {"name", g->meta.name},
                      {"description", g->meta.description},
                      {"created_at", g->meta.created_at}, {"updated_at", g->meta.updated_at},
                      {"nodes", json::array()}, {"edges", json::array()}};
        for (auto& n : g->nodes) {
            j["nodes"].push_back(json{{"id", n.id}, {"type", n.type}, {"label", n.label},
                                       {"config", n.config_json},
                                       {"position", json{{"x", n.position_x}, {"y", n.position_y}}}});
        }
        for (auto& e : g->edges) {
            j["edges"].push_back(json{{"id", e.id}, {"source", e.source_node},
                                       {"source_port", e.source_port},
                                       {"target", e.target_node}, {"target_port", e.target_port}});
        }
        OmniNet::HTTP::Response r = OmniNet::HTTP::Response::ok();
        r.setContentType("application/json");
        r.setBody(j.dump());
        resp = r;
    });

    // PUT /api/graphs/:id
    server.put("/api/graphs/:id", [&store](const OmniNet::HTTP::Request& req,
                                                    OmniNet::HTTP::Response& resp) {
        std::string id{req.pathParam(0)};
        auto body = parse_body(req.bodyAsString());
        if (!body) { resp = OmniNet::HTTP::Response::badRequest("invalid JSON body"); return; }
        std::string name;
        if (body->contains("name") && (*body)["name"].is_string())
            name = (*body)["name"].get<std::string>();
        std::string description;
        if (body->contains("description") && (*body)["description"].is_string())
            description = (*body)["description"].get<std::string>();
        try {
            store.update_graph(id, name, description);
            OmniNet::HTTP::Response r = OmniNet::HTTP::Response::ok();
            r.setContentType("application/json");
            r.setBody(json{{"updated", true}}.dump());
            resp = r;
        } catch (const std::exception& e) {
            resp = OmniNet::HTTP::Response::badRequest(e.what());
        }
    });

    // DELETE /api/graphs/:id
    server.delete_("/api/graphs/:id", [&store](const OmniNet::HTTP::Request& req,
                                                         OmniNet::HTTP::Response& resp) {
        std::string id{req.pathParam(0)};
        try {
            store.delete_graph(id);
            OmniNet::HTTP::Response r = OmniNet::HTTP::Response::ok();
            r.setContentType("application/json");
            r.setBody(json{{"deleted", true}}.dump());
            resp = r;
        } catch (const std::exception& e) {
            resp = OmniNet::HTTP::Response::badRequest(e.what());
        }
    });

    // POST /api/graphs/:gid/nodes
    server.post("/api/graphs/:gid/nodes", [&store](const OmniNet::HTTP::Request& req,
                                                              OmniNet::HTTP::Response& resp) {
        const std::string gid0{req.pathParam(0)}; (void)gid0;
        auto body = parse_body(req.bodyAsString());
        if (!body) { resp = OmniNet::HTTP::Response::badRequest("invalid JSON body"); return; }
        if (!body->contains("type") || !(*body)["type"].is_string() ||
            !body->contains("label") || !(*body)["label"].is_string()) {
            resp = OmniNet::HTTP::Response::badRequest("missing 'type' or 'label' field"); return;
        }
        std::string type = (*body)["type"].get<std::string>();
        std::string label = (*body)["label"].get<std::string>();
        json config = body->contains("config") ? (*body)["config"] : json::object();
        double px = 0, py = 0;
        if (body->contains("position")) {
            if ((*body)["position"].contains("x")) px = (*body)["position"]["x"].get<double>();
            if ((*body)["position"].contains("y")) py = (*body)["position"]["y"].get<double>();
        }
        try {
            std::string gid{req.pathParam(0)};
            std::string nid = store.add_node(gid, type, label, config, px, py);
            OmniNet::HTTP::Response r = OmniNet::HTTP::Response::created(nid);
            r.setContentType("application/json");
            r.setBody(json{{"id", nid}}.dump());
            resp = r;
        } catch (const std::exception& e) {
            resp = OmniNet::HTTP::Response::badRequest(e.what());
        }
    });

    // PUT /api/graphs/:gid/nodes/:nid
    server.put("/api/graphs/:gid/nodes/:nid", [&store](const OmniNet::HTTP::Request& req,
                                                                  OmniNet::HTTP::Response& resp) {
        auto seg = path_segments(req.path());
        if (seg.size() < 5) { resp = OmniNet::HTTP::Response::badRequest("bad path"); return; }
        std::string nid = seg[4];
        auto body = parse_body(req.bodyAsString());
        if (!body) { resp = OmniNet::HTTP::Response::badRequest("invalid JSON body"); return; }
        std::string label;
        if (body->contains("label") && (*body)["label"].is_string())
            label = (*body)["label"].get<std::string>();
        json config = body->contains("config") ? (*body)["config"] : json::object();
        double px = 0, py = 0;
        if (body->contains("position")) {
            if ((*body)["position"].contains("x")) px = (*body)["position"]["x"].get<double>();
            if ((*body)["position"].contains("y")) py = (*body)["position"]["y"].get<double>();
        }
        try {
            store.update_node(nid, label, config, px, py);
            OmniNet::HTTP::Response r = OmniNet::HTTP::Response::ok();
            r.setContentType("application/json");
            r.setBody(json{{"updated", true}}.dump());
            resp = r;
        } catch (const std::exception& e) {
            resp = OmniNet::HTTP::Response::badRequest(e.what());
        }
    });

    // DELETE /api/graphs/:gid/nodes/:nid
    server.delete_("/api/graphs/:gid/nodes/:nid",
        [&store](const OmniNet::HTTP::Request& req, OmniNet::HTTP::Response& resp) {
        auto seg = path_segments(req.path());
        if (seg.size() < 5) { resp = OmniNet::HTTP::Response::badRequest("bad path"); return; }
        std::string nid = seg[4];
        try {
            store.delete_node(nid);
            OmniNet::HTTP::Response r = OmniNet::HTTP::Response::ok();
            r.setContentType("application/json");
            r.setBody(json{{"deleted", true}}.dump());
            resp = r;
        } catch (const std::exception& e) {
            resp = OmniNet::HTTP::Response::badRequest(e.what());
        }
    });

    // POST /api/graphs/:gid/edges
    server.post("/api/graphs/:gid/edges", [&store](const OmniNet::HTTP::Request& req,
                                                              OmniNet::HTTP::Response& resp) {
        const std::string gid0{req.pathParam(0)}; (void)gid0;
        auto body = parse_body(req.bodyAsString());
        if (!body) { resp = OmniNet::HTTP::Response::badRequest("invalid JSON body"); return; }
        if (!body->contains("source") || !(*body)["source"].is_string() ||
            !body->contains("target") || !(*body)["target"].is_string()) {
            resp = OmniNet::HTTP::Response::badRequest("missing 'source' or 'target' field"); return;
        }
        std::string sn = (*body)["source"].get<std::string>();
        std::string sp = "out";
        if (body->contains("source_port") && (*body)["source_port"].is_string())
            sp = (*body)["source_port"].get<std::string>();
        std::string tn = (*body)["target"].get<std::string>();
        std::string tp = "in";
        if (body->contains("target_port") && (*body)["target_port"].is_string())
            tp = (*body)["target_port"].get<std::string>();
        try {
            std::string gid{req.pathParam(0)};
            std::string eid = store.add_edge(gid, sn, sp, tn, tp);
            if (eid.empty()) {
                resp = OmniNet::HTTP::Response::badRequest("edge could not be created"); return;
            }
            OmniNet::HTTP::Response r = OmniNet::HTTP::Response::created(eid);
            r.setContentType("application/json");
            r.setBody(json{{"id", eid}}.dump());
            resp = r;
        } catch (const std::exception& e) {
            resp = OmniNet::HTTP::Response::badRequest(e.what());
        }
    });

    // DELETE /api/graphs/:gid/edges/:eid
    server.delete_("/api/graphs/:gid/edges/:eid",
        [&store](const OmniNet::HTTP::Request& req, OmniNet::HTTP::Response& resp) {
        auto seg = path_segments(req.path());
        if (seg.size() < 5) { resp = OmniNet::HTTP::Response::badRequest("bad path"); return; }
        std::string eid = seg[4];
        try {
            store.delete_edge(eid);
            OmniNet::HTTP::Response r = OmniNet::HTTP::Response::ok();
            r.setContentType("application/json");
            r.setBody(json{{"deleted", true}}.dump());
            resp = r;
        } catch (const std::exception& e) {
            resp = OmniNet::HTTP::Response::badRequest(e.what());
        }
    });
}
