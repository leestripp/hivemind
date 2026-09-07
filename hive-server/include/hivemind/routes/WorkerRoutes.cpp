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

    // ── Model-registry unification (Phase 2) ──────────────────────
    // Hub RELAYS model/secret management to the worker over the channel.
    // Nothing here is persisted hub-side — models.json/secrets.json stay
    // worker-local; these routes are admin-scope in main.cpp's middleware.
    server.get("/api/workers/:name/models",
        [&channel](const OmniNet::HTTP::Request& req,
                   OmniNet::HTTP::Response& resp) {
            auto segs = path_segments(req.path());
            if (segs.size() < 3) { send_error(resp, 400, "bad path"); return; }
            auto r = channel.list_models_detail(segs[2]);
            if (!r) { send_error(resp, 503, "worker offline"); return; }
            if (!r->error.empty()) { send_error(resp, 502, r->error); return; }
            json out = r->data;
            out["worker"] = segs[2];
            send_json(resp, out);
        });

    server.post("/api/workers/:name/models",
        [&channel](const OmniNet::HTTP::Request& req,
                   OmniNet::HTTP::Response& resp) {
            auto segs = path_segments(req.path());
            if (segs.size() < 3) { send_error(resp, 400, "bad path"); return; }
            json body = json::parse(req.body(), nullptr, false);
            if (body.is_discarded()) { send_error(resp, 400, "bad json"); return; }
            const std::string name = body.value("name", "");
            if (name.empty()) { send_error(resp, 400, "name required"); return; }
            json entry = body;
            entry.erase("name");
            try {
                auto r = channel.upsert_model(segs[2], name, entry);
                if (!r) { send_error(resp, 503, "worker offline"); return; }
                if (!r->error.empty()) { send_error(resp, 502, r->error); return; }
                send_json(resp, json{{"ok", true}, {"worker", segs[2]},
                                     {"model", name}});
            } catch (const std::exception& e) {
                send_error(resp, 502, e.what());
            }
        });

    server.delete_("/api/workers/:name/models/:model",
        [&channel](const OmniNet::HTTP::Request& req,
                   OmniNet::HTTP::Response& resp) {
            auto segs = path_segments(req.path());
            if (segs.size() < 5) { send_error(resp, 400, "bad path"); return; }
            auto r = channel.delete_model(segs[2], segs[4]);
            if (!r) { send_error(resp, 503, "worker offline"); return; }
            if (!r->error.empty()) { send_error(resp, 502, r->error); return; }
            send_json(resp, json{{"ok", true}, {"worker", segs[2]},
                                 {"deleted", segs[4]}});
        });

    // Write-only: the key transits the channel to the worker's
    // secrets.json (0600) and is never stored nor returned by the hub.
    server.post("/api/workers/:name/secrets",
        [&channel](const OmniNet::HTTP::Request& req,
                   OmniNet::HTTP::Response& resp) {
            auto segs = path_segments(req.path());
            if (segs.size() < 3) { send_error(resp, 400, "bad path"); return; }
            json body = json::parse(req.body(), nullptr, false);
            if (body.is_discarded()) { send_error(resp, 400, "bad json"); return; }
            const std::string provider = body.value("provider", "");
            const std::string key      = body.value("key", "");
            if (provider.empty() || key.empty()) {
                send_error(resp, 400, "provider and key required"); return;
            }
            try {
                auto r = channel.set_secret(segs[2], provider, key);
                if (!r) { send_error(resp, 503, "worker offline"); return; }
                if (!r->error.empty()) { send_error(resp, 502, r->error); return; }
                send_json(resp, json{{"ok", true}, {"worker", segs[2]},
                                     {"provider", provider}});
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
