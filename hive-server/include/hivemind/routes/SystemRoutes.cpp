#include "SystemRoutes.h"
#include "RouteUtil.h"

#include "hivemind/AgentStore.h"
#include "hivemind/ContainerManager.h"
#include "hivemind/ExecAuditStore.h"
#include "hivemind/SystemMonitor.h"
#include "hivemind/ThreadStore.h"

#include <chrono>
#include <ctime>

using OmniNet::HTTP::Request;
using OmniNet::HTTP::Response;
using nlohmann::json;
namespace routes = hivemind::routes;

namespace {
/// ISO-8601 UTC cutoff for "now minus hours".
std::string iso_hours_ago(int hours) {
    const std::time_t t = std::time(nullptr) - std::time_t(hours) * 3600;
    std::tm tm{};
    gmtime_r(&t, &tm);
    char buf[24];
    std::strftime(buf, sizeof buf, "%Y-%m-%dT%H:%M:%S", &tm);
    return buf;
}
} // namespace

void register_system_routes(OmniNet::HTTP::Server& server,
                            SystemMonitor& monitor,
                            AgentStore& agents,
                            ThreadStore& threads) {

    server.get("/api/system",
        [&monitor](const Request&, Response& resp) {
            routes::send_json(resp, monitor.snapshot());
        });

    server.get("/api/system/history",
        [&monitor](const Request& req, Response& resp) {
            int minutes = 60;
            if (auto m = req.queryParameter("minutes"))
                minutes = std::atoi(std::string(*m).c_str());
            routes::send_json(resp, monitor.history(minutes));
        });

    server.get("/api/system/agents",
        [&agents, &threads](const Request&, Response& resp) {
            const std::string since = iso_hours_ago(24);

            std::unordered_map<std::string, ExecAuditStore::AgentStats> exec;
            try {
                for (auto& a : ContainerManager::exec_audit().stats_since(since))
                    exec[a.agent_id] = std::move(a);
            } catch (...) { /* audit db missing → zeros */ }

            auto msgs = threads.message_counts_since(since);

            json arr = json::array();
            for (auto& a : agents.list_agents()) {
                json row{{"id", a.id}, {"name", a.name},
                         {"worker", a.worker},
                         {"execs_24h", int64_t{0}},
                         {"exec_timeouts_24h", int64_t{0}},
                         {"exec_avg_ms_24h", int64_t{0}},
                         {"messages_sent_24h", int64_t{0}},
                         {"last_exec", ""}};
                if (auto it = exec.find(a.id); it != exec.end()) {
                    row["execs_24h"]          = it->second.count;
                    row["exec_timeouts_24h"]  = it->second.timeouts;
                    row["exec_avg_ms_24h"]    = it->second.avg_ms;
                    row["last_exec"]          = it->second.last_ts;
                }
                if (auto it = msgs.find(a.id); it != msgs.end())
                    row["messages_sent_24h"] = it->second.agent;
                arr.push_back(std::move(row));
            }
            // user message total for context
            int64_t user_msgs = 0;
            for (auto& [id, c] : msgs) user_msgs += c.user;
            routes::send_json(resp, json{{"since", since},
                                         {"user_messages_24h", user_msgs},
                                         {"agents", arr}});
        });
}
