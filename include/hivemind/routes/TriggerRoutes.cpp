// =============================================================================
// include/hivemind/routes/TriggerRoutes.cpp
// =============================================================================
// Webhook triggers: POST /api/hooks/:gid/:secret fires the graph with the
// request body as run inputs, using the same start_graph_run() plumbing as
// the REST /run route. Runner is untouched — triggers just inject inputs.
// =============================================================================

#include "hivemind/routes/TriggerRoutes.h"
#include "hivemind/routes/RunRoutes.h"   // start_graph_run
#include "hivemind/GraphStore.h"
#include "hivemind/Schedule.h"
#include "hivemind/ExecutorRegistry.h"

#include <nlohmann/json.hpp>

#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

using json = nlohmann::json;

namespace {

std::vector<std::string> path_segments(std::string_view path) {
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

json trigger_to_json(const Trigger& t, const std::string& base_url_hint = "") {
    json j{
        {"id",            t.id},
        {"graph_id",      t.graph_id},
        {"kind",          t.kind},
        {"enabled",       t.enabled},
        {"created_at",    t.created_at},
        {"last_fired_at", t.last_fired_at},
        {"next_fire_at",  t.next_fire_at},
    };
    if (t.kind == "webhook") {
        j["secret"] = t.secret;
        j["url"]    = "/api/hooks/" + t.graph_id + "/" + t.secret;
    }
    if (t.kind == "schedule") {
        json cfg = json::parse(t.config_json, nullptr, false);
        if (cfg.is_object()) {
            if (cfg.contains("schedule")) {
                j["schedule"]    = cfg["schedule"];
                j["description"] = hm::describe_schedule(cfg["schedule"]);
            }
            if (cfg.contains("inputs")) j["inputs"] = cfg["inputs"];
        }
    }
    return j;
}

void send_json(OmniNet::HTTP::Response& resp, json body,
               OmniNet::HTTP::Response r = OmniNet::HTTP::Response::ok()) {
    r.setContentType("application/json");
    r.setBody(body.dump());
    resp = r;
}

} // namespace

void register_trigger_routes(OmniNet::HTTP::Server& server,
                             GraphStore& store,
                             ExecutorRegistry& registry)
{
    // POST /api/graphs/:gid/triggers — create a trigger
    // Body (optional): {"kind":"webhook"} (default)
    //   or {"kind":"schedule", "schedule":{"type":"daily","time":"09:00"},
    //       "inputs":{"topic":"..."}}
    server.post("/api/graphs/:gid/triggers",
        [&store](const OmniNet::HTTP::Request& req, OmniNet::HTTP::Response& resp) {
            std::string gid{req.pathParam(0)};
            if (!store.get_graph(gid)) {
                resp = OmniNet::HTTP::Response::notFound("graph not found");
                return;
            }
            json body = json::parse(req.bodyAsString(), nullptr, false);
            if (!body.is_object()) body = json::object();
            const std::string kind = body.value("kind", "webhook");
            if (kind != "webhook" && kind != "schedule") {
                resp = OmniNet::HTTP::Response::badRequest("kind must be webhook|schedule");
                return;
            }
            std::string config_json, next_fire_at;
            if (kind == "schedule") {
                if (!body.contains("schedule")) {
                    resp = OmniNet::HTTP::Response::badRequest("schedule config required");
                    return;
                }
                std::string err = hm::validate_schedule(body["schedule"]);
                if (!err.empty()) {
                    resp = OmniNet::HTTP::Response::badRequest(err);
                    return;
                }
                json cfg;
                cfg["schedule"] = body["schedule"];
                if (body.contains("inputs") && body["inputs"].is_object())
                    cfg["inputs"] = body["inputs"];
                config_json = cfg.dump();
                auto nxt = hm::compute_next_fire(body["schedule"],
                                                 hm::now_iso_local(),
                                                 hm::now_iso_local());
                next_fire_at = nxt.value_or("");
                if (next_fire_at.empty()) {
                    resp = OmniNet::HTTP::Response::badRequest(
                        "schedule never fires (in the past?)");
                    return;
                }
            }
            try {
                std::string id = store.create_trigger(gid, kind, config_json, next_fire_at);
                // Read it back so the response has the generated secret.
                for (auto& t : store.list_triggers(gid)) {
                    if (t.id == id) {
                        send_json(resp, trigger_to_json(t),
                                  OmniNet::HTTP::Response::created(id));
                        return;
                    }
                }
                send_json(resp, json{{"id", id}},
                          OmniNet::HTTP::Response::created(id));
            } catch (const std::exception& e) {
                send_json(resp, json{{"error", e.what()}},
                          OmniNet::HTTP::Response::badRequest(e.what()));
            }
        });

    // GET /api/triggers — list ALL triggers with workflow names
    server.get("/api/triggers",
        [&store](const OmniNet::HTTP::Request& req, OmniNet::HTTP::Response& resp) {
            (void)req;
            json arr = json::array();
            for (auto& t : store.list_all_triggers()) {
                json j = trigger_to_json(t);
                auto meta = store.get_graph_meta(t.graph_id);
                j["graph_name"] = meta ? meta->name : "(deleted)";
                arr.push_back(std::move(j));
            }
            send_json(resp, arr);
        });

    // PUT /api/triggers/:id — enable/disable (schedule recompute on re-enable)
    server.put("/api/triggers/:id",
        [&store](const OmniNet::HTTP::Request& req, OmniNet::HTTP::Response& resp) {
            auto seg = path_segments(req.path());
            if (seg.size() < 3) { resp = OmniNet::HTTP::Response::badRequest("bad path"); return; }
            std::string id = seg[2];
            json body = json::parse(req.bodyAsString(), nullptr, false);
            if (!body.is_object() || !body.contains("enabled") || !body["enabled"].is_boolean()) {
                resp = OmniNet::HTTP::Response::badRequest("expected {enabled: bool}");
                return;
            }
            try {
                store.set_trigger_enabled(id, body["enabled"].get<bool>());
                send_json(resp, json{{"id", id}, {"enabled", body["enabled"]}});
            } catch (const std::exception& e) {
                resp = OmniNet::HTTP::Response::notFound(e.what());
            }
        });

    // GET /api/graphs/:gid/triggers — list triggers for a graph
    server.get("/api/graphs/:gid/triggers",
        [&store](const OmniNet::HTTP::Request& req, OmniNet::HTTP::Response& resp) {
            std::string gid{req.pathParam(0)};
            json arr = json::array();
            for (auto& t : store.list_triggers(gid))
                arr.push_back(trigger_to_json(t));
            send_json(resp, arr);
        });

    // DELETE /api/triggers/:id — delete a trigger
    server.delete_("/api/triggers/:id",
        [&store](const OmniNet::HTTP::Request& req, OmniNet::HTTP::Response& resp) {
            // /api/triggers/:id → segments [api, triggers, id]
            auto seg = path_segments(req.path());
            if (seg.size() < 3) {
                resp = OmniNet::HTTP::Response::badRequest("bad path");
                return;
            }
            try {
                store.delete_trigger(seg[2]);
                send_json(resp, json{{"deleted", seg[2]}});
            } catch (const std::exception& e) {
                send_json(resp, json{{"error", e.what()}},
                          OmniNet::HTTP::Response::badRequest(e.what()));
            }
        });

    // POST /api/hooks/:gid/:secret — fire the webhook: start a run
    server.post("/api/hooks/:gid/:secret",
        [&store, &registry](const OmniNet::HTTP::Request& req,
                            OmniNet::HTTP::Response& resp) {
            // /api/hooks/:gid/:secret → segments [api, hooks, gid, secret]
            auto seg = path_segments(req.path());
            if (seg.size() < 4) {
                resp = OmniNet::HTTP::Response::badRequest("bad path");
                return;
            }
            const std::string& gid = seg[2];
            const std::string& secret = seg[3];

            auto trig = store.get_trigger(gid, secret);
            if (!trig) {
                resp = OmniNet::HTTP::Response::notFound("unknown webhook");
                return;
            }
            if (!trig->enabled) {
                resp = OmniNet::HTTP::Response::badRequest("trigger disabled");
                return;
            }

            auto g = store.get_graph(gid);
            if (!g) {
                resp = OmniNet::HTTP::Response::notFound("graph not found");
                return;
            }

            // Body → run inputs.
            std::unordered_map<std::string, std::string> inputs;
            std::string raw{req.bodyAsString()};
            if (!raw.empty()) {
                try {
                    json body = json::parse(raw);
                    if (body.is_object()) {
                        for (auto& [k, v] : body.items()) {
                            inputs[k] = v.is_string()
                                      ? v.get<std::string>()
                                      : v.dump();
                        }
                    } else {
                        inputs["body"] = body.is_string()
                                       ? body.get<std::string>()
                                       : body.dump();
                    }
                } catch (...) {
                    inputs["body"] = raw;   // not JSON — pass through raw
                }
            }

            try {
                std::string run_id = start_graph_run(store, registry,
                                                     std::move(*g),
                                                     std::move(inputs));
                store.touch_trigger(trig->id);
                send_json(resp, json{
                    {"run_id",     run_id},
                    {"graph_id",   gid},
                    {"trigger_id", trig->id},
                    {"status",     "running"},
                    {"output_url", "/api/runs/" + run_id + "/output"},
                }, OmniNet::HTTP::Response::created(run_id));
            } catch (const std::exception& e) {
                send_json(resp, json{{"error", e.what()}},
                          OmniNet::HTTP::Response::badRequest(e.what()));
            }
        });
}
