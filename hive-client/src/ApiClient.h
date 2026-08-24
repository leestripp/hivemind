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

    /// Phase 9: bearer key sent on every request; base URL configurable
    /// for remote servers.
    void set_api_key(std::string key) { api_key_ = std::move(key); }
    void set_base_url(std::string url) { base_ = std::move(url); }
    const std::string& api_key() const { return api_key_; }

    // ── roster ────────────────────────────────────────────────────────────
    nlohmann::json list_agents();                                    // GET /api/agents
    nlohmann::json create_agent(const std::string& name, const std::string& role,
                                const std::string& model,
                                const std::string& workspace_mode,
                                bool require_approval,
                                const std::string& avatar = "");       // POST /api/agents
    nlohmann::json update_agent(const std::string& id,
                                const std::string& name, const std::string& role,
                                const std::string& model,
                                const std::string& workspace_mode,
                                bool require_approval,
                                const std::string& avatar);            // PATCH /api/agents/:id
    nlohmann::json delete_agent(const std::string& id,
                                const std::string& reassign_to = "");  // DELETE /api/agents/:id[?reassign_to=]
    nlohmann::json list_routines();                                    // GET /api/routines
    nlohmann::json create_routine(const std::string& agent_id,
                                  const std::string& name,
                                  const std::string& prompt,
                                  const nlohmann::json& schedule);     // POST /api/routines
    nlohmann::json container_status(const std::string& agent_id);    // GET …/container/status
    nlohmann::json container_start(const std::string& agent_id);     // POST …/container/start
    nlohmann::json container_stop(const std::string& agent_id);      // POST …/container/stop

    // ── skills (Phase 14.2) ───────────────────────────────────────────────
    nlohmann::json list_global_skills();                              // GET /api/skills
    nlohmann::json agent_global_skills(const std::string& agent_id);  // GET /api/agents/:id/global-skills
    nlohmann::json set_global_skill(const std::string& agent_id,      // POST /api/agents/:id/global-skills
                                    const std::string& name, bool enabled);
    nlohmann::json list_agent_skills(const std::string& agent_id);    // GET /api/agents/:id/skills (local)

    // ── threads / chat ────────────────────────────────────────────────────
    nlohmann::json list_threads();                                   // GET /api/threads
    nlohmann::json create_thread(const std::string& title,
                                 const std::vector<std::string>& agent_ids,
                                 bool reuse_dm = false);
    nlohmann::json list_messages(const std::string& thread_id,
                                 const std::string& after_id = "");
    nlohmann::json post_message(const std::string& thread_id,
                                const std::string& body,
                                const nlohmann::json& attachments
                                    = nlohmann::json::array());
    /// Phase 18: raw-binary upload → <workspace>/attachments/<thread>/.
    /// Returns the server's attachment metadata {name, path, host_path, size}.
    nlohmann::json upload_attachment(const std::string& thread_id,
                                     const std::string& filename,
                                     const std::string& bytes);
    nlohmann::json activity(const std::string& thread_id);           // GET …/activity
    void cancel_thread(const std::string& thread_id);      // POST …/cancel
    void delete_thread(const std::string& thread_id);      // DELETE …/:id

    // ── approvals ─────────────────────────────────────────────────────────
    nlohmann::json list_approvals(const std::string& status = "pending");
    nlohmann::json resolve_approval(const std::string& id, bool approve);

    // ── misc ──────────────────────────────────────────────────────────────
    nlohmann::json models();                                         // GET /api/models
    nlohmann::json providers();                                      // GET /api/providers
    bool healthy();                                                  // GET /api/health

private:
    nlohmann::json request(const std::string& method, const std::string& path,
                           const nlohmann::json* body = nullptr);
    std::string base_;
    std::string api_key_;
};

} // namespace hive
