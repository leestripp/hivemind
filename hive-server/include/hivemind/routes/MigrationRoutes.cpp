// =============================================================================
// MigrationRoutes — see MigrationRoutes.h (Phase 73, Plan.md). Thin HTTP
// skin over MigrationService; all policy lives in the service.
// =============================================================================

#include "hivemind/routes/MigrationRoutes.h"

#include <stdexcept>

#include <nlohmann/json.hpp>

#include "hivemind/MigrationService.h"
#include "hivemind/routes/RouteUtil.h"

namespace {

using json = nlohmann::json;
using OmniNet::HTTP::Request;
using OmniNet::HTTP::Response;
using hivemind::Migration;

json job_json(const Migration& m) {
    return {{"id", m.id},
            {"agent_id", m.agent_id},
            {"agent_name", m.agent_name},
            {"source_worker", m.source_worker},
            {"source_type", m.source_type},
            {"target_worker", m.target_worker},
            {"target_type", m.target_type},
            {"state", m.state},
            {"error", m.error},
            {"retryable", m.retryable},
            {"progress", m.progress},
            {"export_size", m.export_size},
            {"created_at", m.created_at},
            {"updated_at", m.updated_at}};
}

}  // namespace

void register_migration_routes(OmniNet::HTTP::Server& server,
                               hivemind::MigrationService& svc) {
    // Multi-param path — OmniNet pathParam is unreliable here (RouteUtil
    // gotcha); parse positional segments instead.
    server.post("/api/agents/:id/migrate", [&svc](const Request& req,
                                                  Response& resp) {
        const auto segs = hivemind::routes::path_segments(req.path());
        if (segs.size() != 4 || segs[0] != "api" || segs[1] != "agents" ||
            segs[3] != "migrate")
            return false;  // not ours; let other handlers match
        const std::string agent_id = segs[2];

        // nlohmann takes the body span directly; discarded = bad JSON.
        json body = json::parse(req.body(), nullptr, /*exceptions=*/false);
        if (body.is_discarded() || !body.is_object())
            return hivemind::routes::send_error(resp, 400, "invalid JSON"), true;
        const std::string target_worker = body.value("target_worker", "");
        const std::string target_type = body.value("target_type", "container");
        const bool force = body.value("force", false);
        const bool no_rollback = body.value("no_rollback", false);

        try {
            const std::string job_id = svc.begin(agent_id, target_worker,
                                                 target_type, force,
                                                 no_rollback);
            return hivemind::routes::send_json(resp, {{"job_id", job_id}}, 202),
                   true;
        } catch (const std::invalid_argument& e) {
            const std::string msg = e.what();
            const int status =
                msg.rfind("no such agent", 0) == 0 ? 404 : 400;
            return hivemind::routes::send_error(resp, status, msg), true;
        } catch (const std::exception& e) {
            return hivemind::routes::send_error(resp, 500, e.what()), true;
        }
    });

    server.get("/api/migrations", [&svc](const Request& req, Response& resp) {
        const auto q = req.query();
        const std::string agent =
            q.get("agent") ? std::string{*q.get("agent")} : "";
        const std::string state =
            q.get("state") ? std::string{*q.get("state")} : "";
        json arr = json::array();
        for (const auto& m : svc.store().list(agent, 200)) {
            if (!state.empty() && m.state != state) continue;
            arr.push_back(job_json(m));
        }
        return hivemind::routes::send_json(resp, {{"migrations", arr}}), true;
    });

    server.get("/api/migrations/:id", [&svc](const Request& req,
                                             Response& resp) {
        const auto segs = hivemind::routes::path_segments(req.path());
        if (segs.size() != 3 || segs[0] != "api" || segs[1] != "migrations")
            return false;
        auto m = svc.store().get(segs[2]);
        if (!m) return hivemind::routes::send_error(resp, 404, "no such migration"), true;
        return hivemind::routes::send_json(resp, job_json(*m)), true;
    });

    server.post("/api/migrations/:id/cancel", [&svc](const Request& req,
                                                     Response& resp) {
        const auto segs = hivemind::routes::path_segments(req.path());
        if (segs.size() != 4 || segs[0] != "api" || segs[1] != "migrations" ||
            segs[3] != "cancel")
            return false;
        try {
            svc.cancel(segs[2]);
            return hivemind::routes::send_json(resp, {{"ok", true}}), true;
        } catch (const std::invalid_argument& e) {
            return hivemind::routes::send_error(resp, 400, e.what()), true;
        } catch (const std::exception& e) {
            return hivemind::routes::send_error(resp, 500, e.what()), true;
        }
    });
}
