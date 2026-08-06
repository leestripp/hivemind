// =============================================================================
// include/hivemind/routes/RunRoutes.cpp
// -----------------------------------------------------------------------------
// REST surface for graph runs:
//   POST /api/graphs/:gid/run              start a run, returns 202 + run_id
//   GET  /api/graphs/:gid/runs/:rid        poll run status + accumulated output
//   GET  /api/runs/:rid/output             tail the live output buffer
//
// Streaming is via HTTP polling in v1. A WebSocket at /ws/runs can be
// added later via HTTP::Server::registerProtocolSwitch (see docs/api/
// http-server.md §Request Routing → Wildcards, and docs/api/websocket.md).
// =============================================================================

#include "hivemind/GraphStore.h"
#include "hivemind/GraphRunner.h"
#include "hivemind/ExecutorRegistry.h"

#include <OmniNet.h>
#include <nlohmann/json.hpp>

#include <atomic>
#include <map>
#include <mutex>
#include <thread>
#include <unordered_map>
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

// ─── Per-run live output buffer ─────────────────────────────────────────
// Each run accumulates stdout-like text as nodes complete. The buffer
// is fetched via GET /api/runs/:rid/output (long polling is fine for v1).

struct RunState {
    std::mutex                                  m;
    std::string                                  output;        // appended text
    bool                                        finished = false;
    std::string                                 status  = "running"; // running|completed|failed
    std::string                                 error;
};

static std::unordered_map<std::string, std::shared_ptr<RunState>> g_runs;
static std::mutex g_runs_mtx;

static std::shared_ptr<RunState> get_or_create_run(const std::string& run_id) {
    std::lock_guard<std::mutex> lk{g_runs_mtx};
    auto it = g_runs.find(run_id);
    if (it != g_runs.end()) return it->second;
    auto rs = std::make_shared<RunState>();
    g_runs[run_id] = rs;
    return rs;
}

static std::shared_ptr<RunState> get_run_state(const std::string& run_id) {
    std::lock_guard<std::mutex> lk{g_runs_mtx};
    auto it = g_runs.find(run_id);
    if (it == g_runs.end()) return nullptr;
    return it->second;
}

static void append_run_output(const std::string& run_id, const std::string& text) {
    auto rs = get_run_state(run_id);
    if (!rs) return;
    std::lock_guard<std::mutex> lk{rs->m};
    rs->output += text;
}

static void finish_run(const std::string& run_id, const std::string& status,
                      const std::string& error = "") {
    auto rs = get_run_state(run_id);
    if (!rs) return;
    std::lock_guard<std::mutex> lk{rs->m};
    rs->finished = true;
    rs->status = status;
    rs->error = error;
}

// ─── Route registration ────────────────────────────────────────────────

void register_run_routes(OmniNet::HTTP::Server& server,
                         GraphStore& store,
                         ExecutorRegistry& registry)
{
    // POST /api/graphs/:gid/run — start a run
    server.post("/api/graphs/:gid/run",
        [&](const OmniNet::HTTP::Request& req, OmniNet::HTTP::Response& resp) {
            std::string gid{req.pathParam(0)};
            auto g = store.get_graph(gid);
            if (!g) { resp = OmniNet::HTTP::Response::notFound("graph not found"); return; }

            std::unordered_map<std::string, std::string> run_inputs;
            auto body = parse_body(req.bodyAsString());
            if (body && body->contains("inputs") && (*body)["inputs"].is_object()) {
                for (auto& [k, v] : (*body)["inputs"].items()) {
                    if (v.is_string()) run_inputs[k] = v.get<std::string>();
                }
            }

            std::string run_id;
            try {
                run_id = store.create_run(gid);
            } catch (const std::exception& e) {
                resp = OmniNet::HTTP::Response::badRequest(e.what()); return;
            }

            // Allocate state before spawning the runner so the
            // HTTP response can refer to a valid run_id.
            get_or_create_run(run_id);

            // Spawn the runner on a detached thread.
            std::thread t{[&, gr=std::move(*g), run_id,
                           inputs=std::move(run_inputs)]() mutable {
                GraphRunner runner{store, registry, gr, run_id, std::move(inputs)};
                runner.set_event_callback([&run_id](json event) {
                    append_run_output(run_id, event.dump() + "\n");
                });
                try {
                    runner.run_sync();
                    if (runner.failed()) {
                        finish_run(run_id, "failed", runner.fail_error());
                        try { store.update_run(run_id, "failed", runner.fail_error()); }
                        catch (...) {}
                    } else {
                        finish_run(run_id, "completed");
                        store.update_run(run_id, "completed");
                    }
                } catch (const std::exception& e) {
                    append_run_output(run_id,
                        json{{"event","node_failed"},
                             {"error", e.what()}}.dump() + "\n");
                    finish_run(run_id, "failed", e.what());
                    try { store.update_run(run_id, "failed", e.what()); }
                    catch (...) {}
                }
            }};
            t.detach();

            json out{
                {"run_id", run_id},
                {"graph_id", gid},
                {"status", "running"},
                {"output_url", "/api/runs/" + run_id + "/output"},
            };
            resp = OmniNet::HTTP::Response::created(run_id);
            resp.setContentType("application/json");
            resp.setBody(out.dump());
        });

    // GET /api/graphs/:gid/runs/:rid — full run status + DB results
    server.get("/api/graphs/:gid/runs/:rid",
        [&store](const OmniNet::HTTP::Request& req, OmniNet::HTTP::Response& resp) {
            // /api/graphs/:gid/runs/:rid → segments [api, graphs, gid, runs, rid]
            auto seg = path_segments(req.path());
            if (seg.size() < 5) { resp = OmniNet::HTTP::Response::badRequest("bad path"); return; }
            std::string rid = seg[4];
            auto run = store.get_run(rid);
            if (!run) { resp = OmniNet::HTTP::Response::notFound("run not found"); return; }
            json results = json::object();
            for (auto& [k, v] : run->results) results[k] = v;
            json out{
                {"run_id", run->run_id},
                {"graph_id", run->graph_id},
                {"status", run->status},
                {"error", run->error},
                {"results", results},
                {"completed_nodes", run->completed_nodes},
            };
            resp = OmniNet::HTTP::Response::ok();
            resp.setContentType("application/json");
            resp.setBody(out.dump());
        });

    // GET /api/runs/:rid/output — tail the live output buffer
    server.get("/api/runs/:rid/output",
        [](const OmniNet::HTTP::Request& req, OmniNet::HTTP::Response& resp) {
            std::string rid{req.pathParam(0)};
            auto rs = get_run_state(rid);
            if (!rs) { resp = OmniNet::HTTP::Response::notFound("run not found"); return; }
            std::string snapshot;
            bool finished;
            std::string status, error;
            {
                std::lock_guard<std::mutex> lk{rs->m};
                snapshot = rs->output;
                finished = rs->finished;
                status   = rs->status;
                error    = rs->error;
            }
            json out{
                {"run_id", rid},
                {"status", status},
                {"finished", finished},
                {"error", error},
                {"output", snapshot},
            };
            resp = OmniNet::HTTP::Response::ok();
            resp.setContentType("application/json");
            resp.setBody(out.dump());
        });
}
