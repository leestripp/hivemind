// include/hivemind/routes/WorkerRoutes.cpp — Phase 30.1 worker registry.
#include "hivemind/routes/WorkerRoutes.h"
#include "hivemind/routes/RouteUtil.h"
#include "hivemind/WorkerStore.h"
#include "hivemind/WorkerChannel.h"

#include <nlohmann/json.hpp>

using json = nlohmann::json;
using hivemind::routes::path_segments;
using hivemind::routes::send_json;
using hivemind::routes::send_error;

void register_worker_routes(OmniNet::HTTP::Server& server,
                            WorkerStore& workers,
                            WorkerChannel& channel) {

    server.get("/api/workers",
        [&workers, &channel](const OmniNet::HTTP::Request&,
                             OmniNet::HTTP::Response& resp) {
            json list = json::array();
            for (auto& w : workers.list()) {
                json caps = json::object();
                try { caps = json::parse(w.caps); } catch (...) {}
                list.push_back({{"name", w.name}, {"caps", caps},
                                {"proto", w.proto},
                                {"last_seen", w.last_seen},
                                {"created_at", w.created_at},
                                {"online", channel.is_online(w.name)}});
            }
            send_json(resp, json{{"workers", list}});
        });

    // v2: model catalog = union of connected workers' models.json
    // (the hub holds no LLM config of its own; workers own credentials).
    server.get("/api/models",
        [&channel](const OmniNet::HTTP::Request&,
                   OmniNet::HTTP::Response& resp) {
            send_json(resp, json{{"models", channel.models_catalog()}});
        });

    // v2 follow-up: set a worker's default model (writes its models.json
    // "default" key via the channel). Agents with model "" resolve to it.
    server.post("/api/workers/:name/default-model",
        [&channel](const OmniNet::HTTP::Request& req,
                   OmniNet::HTTP::Response& resp) {
            auto segs = path_segments(req.path());
            if (segs.size() < 3) { send_error(resp, 400, "bad path"); return; }
            json body = json::parse(req.body(), nullptr, false);
            if (body.is_discarded()) { send_error(resp, 400, "bad json"); return; }
            const std::string model = body.value("model", "");
            if (model.empty()) { send_error(resp, 400, "model required"); return; }
            try {
                auto r = channel.set_default_model(segs[2], model);
                if (!r) { send_error(resp, 503, "worker offline"); return; }
                if (!r->error.empty()) { send_error(resp, 502, r->error); return; }
                send_json(resp, json{{"ok", true}, {"worker", segs[2]},
                                     {"default_model", model}});
            } catch (const std::exception& e) {
                send_error(resp, 502, e.what());
            }
        });

    server.delete_("/api/workers/:name",
        [&workers](const OmniNet::HTTP::Request& req,
                   OmniNet::HTTP::Response& resp) {
            // OmniNet pathParam unreliable for params — parse the path.
            auto segs = path_segments(req.path());
            if (segs.size() < 3) { send_error(resp, 400, "bad path"); return; }
            if (!workers.remove(segs[2])) {
                send_error(resp, 404, "unknown worker");
                return;
            }
            send_json(resp, json{{"ok", true}});
        });
}
