// include/hivemind/routes/WorkerRoutes.cpp — Phase 30.1 worker registry.
#include "hivemind/routes/WorkerRoutes.h"
#include "hivemind/routes/RouteUtil.h"
#include "hivemind/WorkerStore.h"

#include <nlohmann/json.hpp>

using json = nlohmann::json;
using hivemind::routes::path_segments;
using hivemind::routes::send_json;
using hivemind::routes::send_error;

void register_worker_routes(OmniNet::HTTP::Server& server,
                            WorkerStore& workers) {

    server.get("/api/workers",
        [&workers](const OmniNet::HTTP::Request&, OmniNet::HTTP::Response& resp) {
            json list = json::array();
            for (auto& w : workers.list()) {
                json caps = json::object();
                try { caps = json::parse(w.caps); } catch (...) {}
                list.push_back({{"name", w.name}, {"caps", caps},
                                {"proto", w.proto},
                                {"last_seen", w.last_seen},
                                {"created_at", w.created_at},
                                {"online", false}});   // real flag: 30.2
            }
            send_json(resp, json{{"workers", list}});
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
