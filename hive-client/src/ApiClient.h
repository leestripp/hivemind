#pragma once

#include <nlohmann/json.hpp>

#include <functional>
#include <string>

namespace hive {

/**
 * ApiClient — synchronous HTTP+JSON client for the hive-server REST API.
 * All calls are blocking: invoke them from worker threads and marshal
 * results back to the GTK main loop (Glib::Dispatcher / idle).
 */
class ApiClient {
public:
    explicit ApiClient(std::string base_url = "http://127.0.0.1:8421")
        : base_(std::move(base_url)) {}

    // ── roster ────────────────────────────────────────────────────────────
    nlohmann::json list_agents();                                    // GET /api/agents
    nlohmann::json create_agent(const std::string& name, const std::string& role,
                                const std::string& model,
                                const std::string& workspace_mode,
                                bool require_approval);              // POST /api/agents
    nlohmann::json container_status(const std::string& agent_id);    // GET …/container/status

    // ── threads / chat ────────────────────────────────────────────────────
    nlohmann::json list_threads();                                   // GET /api/threads
    nlohmann::json create_thread(const std::string& title,
                                 const std::vector<std::string>& agent_ids);
    nlohmann::json list_messages(const std::string& thread_id,
                                 const std::string& after_id = "");
    nlohmann::json post_message(const std::string& thread_id,
                                const std::string& body);
    nlohmann::json activity(const std::string& thread_id);           // GET …/activity

    // ── approvals ─────────────────────────────────────────────────────────
    nlohmann::json list_approvals(const std::string& status = "pending");
    nlohmann::json resolve_approval(const std::string& id, bool approve);

    // ── misc ──────────────────────────────────────────────────────────────
    nlohmann::json models();                                         // GET /api/models
    bool healthy();                                                  // GET /api/health

private:
    nlohmann::json request(const std::string& method, const std::string& path,
                           const nlohmann::json* body = nullptr);
    std::string base_;
};

} // namespace hive
