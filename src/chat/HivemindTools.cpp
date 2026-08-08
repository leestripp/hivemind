// src/chat/HivemindTools.cpp — HiveMind-control tools for the Chat agent.

#include "hivemind/chat/HivemindTools.h"

#include "hivemind/Graph.h"
#include "hivemind/GraphStore.h"
#include "hivemind/Schedule.h"
#include "hivemind/routes/RunRoutes.h"   // start_graph_run

#include <nlohmann/json.hpp>

using json = nlohmann::json;
using daedalus::BaseTool;
using daedalus::HostToolContext;

namespace {

// ── shared helpers ──────────────────────────────────────────────────────────

json ok(json payload) {
    payload["status"] = "ok";
    return payload;
}
json err(std::string msg) {
    return json{{"status", "error"}, {"error", std::move(msg)}};
}

/// Static knowledge about node types: ports + config schema hints.
json node_type_knowledge() {
    return json::array({
        {{"type", "input"},  {"in", json::array()}, {"out", {"out"}},
         {"config", {{"param_name", "string — key read from the run inputs"},
                     {"value", "string — fallback when no run input"}}}},
        {{"type", "prompt"}, {"in", json::array()}, {"out", {"out"}},
         {"config", {{"value", "string — the static text this node emits"}}}},
        {{"type", "agent"},  {"in", {"in"}}, {"out", {"out"}},
         {"config", {{"model", "string — model name, e.g. MiniMax-M2.7"},
                     {"project", "string — working dir for the agent's tools"},
                     {"system_prompt", "string — optional override"}}}},
        {{"type", "merge"},  {"in", {"in_a", "in_b", "in_c"}}, {"out", {"out"}},
         {"config", {{"strategy", "concat (default)"},
                     {"separator", "string — between inputs, default blank line"}}}},
        {{"type", "loop_start"}, {"in", {"in"}}, {"out", {"out", "loop_count"}},
         {"config", {{"loop_count", "number — iterations (paired with loop_end)"}}}},
        {{"type", "loop_end"}, {"in", {"in", "loop_count"}}, {"out", {"out"}},
         {"config", json::object()}},
        {{"type", "output"}, {"in", {"in"}}, {"out", json::array()},
         {"config", {{"output_name", "string — key in the run results"}}}},
        {{"type", "log"},    {"in", {"in"}}, {"out", json::array()},
         {"config", json::object()}},
    });
}

// ── 1. list_workflows ───────────────────────────────────────────────────────

class ListWorkflows final : public BaseTool {
public:
    explicit ListWorkflows(GraphStore& s) : store_(s) {}
    std::string get_name() const override { return "hivemind_list_workflows"; }
    std::string get_description() const override {
        return "List all HiveMind workflows (node graphs). Pass graph_id for full "
               "node/edge detail of one workflow.";
    }
    json get_definition() const override {
        json params = json::parse(R"json({"type":"object","properties":{"graph_id":{"type":"string","description":"optional — full detail for one workflow"}}})json");
        return json{{"type", "function"}, {"function", {
            {"name", get_name()}, {"description", get_description()},
            {"parameters", std::move(params)}
        }}};
    }
    json execute(const json& args, HostToolContext&) override {
        try {
            if (args.contains("graph_id") && args["graph_id"].is_string()) {
                auto g = store_.get_graph(args["graph_id"].get<std::string>());
                if (!g) return err("workflow not found");
                json nodes = json::array(), edges = json::array();
                for (auto& n : g->nodes)
                    nodes.push_back({{"id", n.id}, {"type", n.type}, {"label", n.label},
                                     {"config", n.config_json}});
                for (auto& e : g->edges)
                    edges.push_back({{"id", e.id},
                                     {"source", e.source_node}, {"source_port", e.source_port},
                                     {"target", e.target_node}, {"target_port", e.target_port}});
                return ok({{"id", g->meta.id}, {"name", g->meta.name},
                           {"nodes", nodes}, {"edges", edges}});
            }
            json arr = json::array();
            for (auto& m : store_.list_graphs())
                arr.push_back({{"id", m.id}, {"name", m.name},
                               {"description", m.description},
                               {"created_at", m.created_at}, {"updated_at", m.updated_at}});
            return ok({{"workflows", arr}, {"count", arr.size()}});
        } catch (const std::exception& e) { return err(e.what()); }
    }
private:
    GraphStore& store_;
};

// ── 2. create_workflow ─────────────────────────────────────────────────────

class CreateWorkflow final : public BaseTool {
public:
    explicit CreateWorkflow(GraphStore& s) : store_(s) {}
    std::string get_name() const override { return "hivemind_create_workflow"; }
    std::string get_description() const override {
        return "Create a new empty HiveMind workflow. Returns its graph_id — then "
               "add nodes and connect them with edges.";
    }
    json get_definition() const override {
        json params = json::parse(R"json({"type":"object","properties":{"name":{"type":"string"},"description":{"type":"string"}},"required":["name"]})json");
        return json{{"type", "function"}, {"function", {
            {"name", get_name()}, {"description", get_description()},
            {"parameters", std::move(params)}
        }}};
    }
    json execute(const json& args, HostToolContext&) override {
        try {
            if (!args.contains("name") || !args["name"].is_string())
                return err("missing required arg: name");
            std::string desc = args.value("description", "");
            std::string id = store_.create_graph(args["name"].get<std::string>(), desc);
            return ok({{"graph_id", id}, {"name", args["name"]}});
        } catch (const std::exception& e) { return err(e.what()); }
    }
private:
    GraphStore& store_;
};

// ── 3. list_node_types ─────────────────────────────────────────────────────

class ListNodeTypes final : public BaseTool {
public:
    std::string get_name() const override { return "hivemind_list_node_types"; }
    std::string get_description() const override {
        return "List the available node types with their input/output ports and "
               "config fields. Call this before building a workflow.";
    }
    json get_definition() const override {
        json params = json::parse(R"json({"type":"object","properties":{}})json");
        return json{{"type", "function"}, {"function", {
            {"name", get_name()}, {"description", get_description()},
            {"parameters", std::move(params)}
        }}};
    }
    json execute(const json&, HostToolContext&) override {
        return ok({{"node_types", node_type_knowledge()}});
    }
};

// ── 4. add_node ────────────────────────────────────────────────────────────

class AddNode final : public BaseTool {
public:
    explicit AddNode(GraphStore& s) : store_(s) {}
    std::string get_name() const override { return "hivemind_add_node"; }
    std::string get_description() const override {
        return "Add a node to a workflow. Type must be one of the known node types "
               "(see hivemind_list_node_types). Returns the new node_id.";
    }
    json get_definition() const override {
        json params = json::parse(R"json({"type":"object","properties":{"graph_id":{"type":"string"},"type":{"type":"string"},"label":{"type":"string"},"config":{"type":"object"},"x":{"type":"number"},"y":{"type":"number"}},"required":["graph_id","type","label"]})json");
        return json{{"type", "function"}, {"function", {
            {"name", get_name()}, {"description", get_description()},
            {"parameters", std::move(params)}
        }}};
    }
    json execute(const json& args, HostToolContext&) override {
        try {
            std::string gid   = args.value("graph_id", "");
            std::string type  = args.value("type", "");
            std::string label = args.value("label", "");
            if (gid.empty() || type.empty() || label.empty())
                return err("missing required args: graph_id, type, label");
            if (Node::ports_for_type(type).empty())
                return err("unknown node type: " + type);
            json config = args.contains("config") && args["config"].is_object()
                          ? args["config"] : json::object();
            double x = args.value("x", 100.0), y = args.value("y", 100.0);
            std::string nid = store_.add_node(gid, type, label, config, x, y);
            return ok({{"node_id", nid}, {"type", type}, {"label", label}});
        } catch (const std::exception& e) { return err(e.what()); }
    }
private:
    GraphStore& store_;
};

// ── 5. connect_nodes ───────────────────────────────────────────────────────

class ConnectNodes final : public BaseTool {
public:
    explicit ConnectNodes(GraphStore& s) : store_(s) {}
    std::string get_name() const override { return "hivemind_connect_nodes"; }
    std::string get_description() const override {
        return "Wire two nodes: creates an edge from source's output port to "
               "target's input port. Ports default to out→in.";
    }
    json get_definition() const override {
        json params = json::parse(R"json({"type":"object","properties":{"graph_id":{"type":"string"},"source_node":{"type":"string"},"source_port":{"type":"string"},"target_node":{"type":"string"},"target_port":{"type":"string"}},"required":["graph_id","source_node","target_node"]})json");
        return json{{"type", "function"}, {"function", {
            {"name", get_name()}, {"description", get_description()},
            {"parameters", std::move(params)}
        }}};
    }
    json execute(const json& args, HostToolContext&) override {
        try {
            std::string gid = args.value("graph_id", "");
            std::string sn  = args.value("source_node", "");
            std::string tn  = args.value("target_node", "");
            std::string sp  = args.value("source_port", "out");
            std::string tp  = args.value("target_port", "in");
            if (gid.empty() || sn.empty() || tn.empty())
                return err("missing required args: graph_id, source_node, target_node");
            std::string eid = store_.add_edge(gid, sn, sp, tn, tp);
            return ok({{"edge_id", eid},
                       {"edge", sn + ":" + sp + " → " + tn + ":" + tp}});
        } catch (const std::exception& e) { return err(e.what()); }
    }
private:
    GraphStore& store_;
};

// ── 6. list_triggers ───────────────────────────────────────────────────────

class ListTriggers final : public BaseTool {
public:
    explicit ListTriggers(GraphStore& s) : store_(s) {}
    std::string get_name() const override { return "hivemind_list_triggers"; }
    std::string get_description() const override {
        return "List all triggers (schedules + webhooks) across workflows.";
    }
    json get_definition() const override {
        json params = json::parse(R"json({"type":"object","properties":{}})json");
        return json{{"type", "function"}, {"function", {
            {"name", get_name()}, {"description", get_description()},
            {"parameters", std::move(params)}
        }}};
    }
    json execute(const json&, HostToolContext&) override {
        try {
            json arr = json::array();
            for (auto& t : store_.list_all_triggers()) {
                json cfg = json::parse(t.config_json, nullptr, false);
                arr.push_back({{"id", t.id}, {"graph_id", t.graph_id},
                               {"kind", t.kind}, {"enabled", t.enabled},
                               {"next_fire_at", t.next_fire_at},
                               {"last_fired_at", t.last_fired_at},
                               {"schedule", cfg.is_object() && cfg.contains("schedule")
                                            ? cfg["schedule"] : json(nullptr)}});
            }
            return ok({{"triggers", arr}, {"count", arr.size()}});
        } catch (const std::exception& e) { return err(e.what()); }
    }
private:
    GraphStore& store_;
};

// ── 7. create_trigger ──────────────────────────────────────────────────────

class CreateTrigger final : public BaseTool {
public:
    explicit CreateTrigger(GraphStore& s) : store_(s) {}
    std::string get_name() const override { return "hivemind_create_trigger"; }
    std::string get_description() const override {
        return "Create a trigger for a workflow. kind='schedule' needs a schedule "
               "object: {type:'once',at:'YYYY-MM-DDTHH:MM'} | {type:'daily',time:'HH:MM'} | "
               "{type:'weekly',days:[1-7 as 0=Sun..6=Sat],time:'HH:MM'} | "
               "{type:'interval',every_minutes:N}. kind='webhook' creates an HTTP hook "
               "(no schedule). Optional inputs object is passed to the workflow run.";
    }
    json get_definition() const override {
        json params = json::parse(R"json({"type":"object","properties":{"graph_id":{"type":"string"},"kind":{"type":"string","enum":["schedule","webhook"]},"schedule":{"type":"object"},"inputs":{"type":"object"}},"required":["graph_id","kind"]})json");
        return json{{"type", "function"}, {"function", {
            {"name", get_name()}, {"description", get_description()},
            {"parameters", std::move(params)}
        }}};
    }
    json execute(const json& args, HostToolContext&) override {
        try {
            std::string gid  = args.value("graph_id", "");
            std::string kind = args.value("kind", "");
            if (gid.empty() || kind.empty())
                return err("missing required args: graph_id, kind");
            if (!store_.get_graph_meta(gid)) return err("workflow not found");

            if (kind == "webhook") {
                std::string id = store_.create_trigger(gid, "webhook", "", "");
                return ok({{"trigger_id", id}, {"kind", "webhook"},
                           {"note", "fire via POST /api/hooks/<graph_id>/<secret> "
                                    "(secret shown in the Triggers page)"}});
            }
            if (kind != "schedule") return err("kind must be 'schedule' or 'webhook'");
            if (!args.contains("schedule") || !args["schedule"].is_object())
                return err("schedule kind requires a schedule object");

            std::string verr = hm::validate_schedule(args["schedule"]);
            if (!verr.empty()) return err(verr);

            json cfg;
            cfg["schedule"] = args["schedule"];
            if (args.contains("inputs") && args["inputs"].is_object())
                cfg["inputs"] = args["inputs"];
            auto nxt = hm::compute_next_fire(args["schedule"],
                                             hm::now_iso_local(), hm::now_iso_local());
            if (!nxt) return err("schedule never fires (in the past?)");
            std::string id = store_.create_trigger(gid, "schedule", cfg.dump(), *nxt);
            return ok({{"trigger_id", id}, {"kind", "schedule"},
                       {"description", hm::describe_schedule(args["schedule"])},
                       {"next_fire_at", *nxt}});
        } catch (const std::exception& e) { return err(e.what()); }
    }
private:
    GraphStore& store_;
};

// ── 8. run_workflow ────────────────────────────────────────────────────────

class RunWorkflow final : public BaseTool {
public:
    RunWorkflow(GraphStore& s, ExecutorRegistry& r) : store_(s), registry_(r) {}
    std::string get_name() const override { return "hivemind_run_workflow"; }
    std::string get_description() const override {
        return "Start a run of a workflow (like pressing Run on the canvas). "
               "Returns a run_id; the run proceeds in the background.";
    }
    json get_definition() const override {
        json params = json::parse(R"json({"type":"object","properties":{"graph_id":{"type":"string"},"inputs":{"type":"object","description":"string values keyed by input-node param_name"}},"required":["graph_id"]})json");
        return json{{"type", "function"}, {"function", {
            {"name", get_name()}, {"description", get_description()},
            {"parameters", std::move(params)}
        }}};
    }
    json execute(const json& args, HostToolContext&) override {
        try {
            std::string gid = args.value("graph_id", "");
            if (gid.empty()) return err("missing required arg: graph_id");
            auto g = store_.get_graph(gid);
            if (!g) return err("workflow not found");
            std::unordered_map<std::string, std::string> inputs;
            if (args.contains("inputs") && args["inputs"].is_object())
                for (auto& [k, v] : args["inputs"].items())
                    if (v.is_string()) inputs[k] = v.get<std::string>();
            std::string rid = start_graph_run(store_, registry_, std::move(*g), std::move(inputs));
            return ok({{"run_id", rid}, {"graph_id", gid}});
        } catch (const std::exception& e) { return err(e.what()); }
    }
private:
    GraphStore&       store_;
    ExecutorRegistry& registry_;
};

}  // namespace

std::vector<std::unique_ptr<BaseTool>>
make_hivemind_tools(GraphStore& store, ExecutorRegistry& registry) {
    std::vector<std::unique_ptr<BaseTool>> tools;
    tools.push_back(std::make_unique<ListWorkflows>(store));
    tools.push_back(std::make_unique<CreateWorkflow>(store));
    tools.push_back(std::make_unique<ListNodeTypes>());
    tools.push_back(std::make_unique<AddNode>(store));
    tools.push_back(std::make_unique<ConnectNodes>(store));
    tools.push_back(std::make_unique<ListTriggers>(store));
    tools.push_back(std::make_unique<CreateTrigger>(store));
    tools.push_back(std::make_unique<RunWorkflow>(store, registry));
    return tools;
}
