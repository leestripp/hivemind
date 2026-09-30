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
    const std::string& base_url() const { return base_; }

    // ── roster ────────────────────────────────────────────────────────────
    nlohmann::json list_agents();                                    // GET /api/agents
    nlohmann::json templates();                                      // GET /api/templates (Phase 19)
    nlohmann::json create_agent(const std::string& name, const std::string& role,
                                const std::string& model,
                                const std::string& workspace_mode,
                                bool require_approval,
                                const std::string& avatar = "",
                                const std::string& worker = "",
                                const std::string& type = "",
                                const std::string& memory = "",
                                double cpus = -1,                     // Phase 69: -1 = default
                                const std::string& thinking_level = ""); // 2026-10-01: "" = inherit from model
    nlohmann::json update_agent(const std::string& id,
                                const std::string& name, const std::string& role,
                                const std::string& model,
                                const std::string& workspace_mode,
                                bool require_approval,
                                const std::string& avatar,
                                const std::string& worker = "",
                                const std::string& type = "",
                                const std::string& memory = "",
                                double cpus = -1,                     // Phase 69: -1 keep, 0 reset
                                const std::string& thinking_level = ""); // 2026-10-01: "" keep, "default" clears, else set
    nlohmann::json list_workers();                                   // GET /api/workers (Phase 30)
    nlohmann::json delete_agent(const std::string& id,
                                const std::string& reassign_to = "");  // DELETE /api/agents/:id[?reassign_to=]
    nlohmann::json list_routines();                                    // GET /api/routines
    nlohmann::json create_routine(const std::string& agent_id,
                                  const std::string& name,
                                  const std::string& prompt,
                                  const nlohmann::json& schedule);     // POST /api/routines
    nlohmann::json update_routine(const std::string& id,
                                  const std::string& name,
                                  const std::string& prompt,
                                  const nlohmann::json& schedule);     // PATCH /api/routines/:id
    void delete_routine(const std::string& id);                        // DELETE /api/routines/:id
    void run_routine(const std::string& id);                           // POST /api/routines/:id/run
    void set_routine_enabled(const std::string& id, bool enabled);     // POST …/enabled
    nlohmann::json screen_start(const std::string& agent_id);
    nlohmann::json screen_status(const std::string& agent_id);
    nlohmann::json agent_start(const std::string& agent_id);       // POST .../start
    nlohmann::json agent_stop(const std::string& agent_id);        // POST .../stop
    nlohmann::json exec_log(const std::string& agent_id,
                            int limit = 200);                        // GET …/exec-log

    // ── skills (Phase 14.2) ───────────────────────────────────────────────
    nlohmann::json list_global_skills();                              // GET /api/skills
    void create_skill(const std::string& name, const std::string& description,
                      const std::string& content);                     // POST /api/skills
    void delete_skill(const std::string& name);                        // DELETE /api/skills/:name
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
    nlohmann::json list_messages_before(const std::string& thread_id,
                                        const std::string& before_id);
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
    void cancel_thread(const std::string& thread_id);
    nlohmann::json cancel_agent(const std::string& agent_id);      // POST …/cancel
    void delete_thread(const std::string& thread_id);
    void clear_thread(const std::string& thread_id);      // DELETE …/:id

    // ── approvals ─────────────────────────────────────────────────────────
    nlohmann::json list_approvals(const std::string& status = "pending");
    nlohmann::json resolve_approval(const std::string& id, bool approve);

    // ── misc ──────────────────────────────────────────────────────────────
    nlohmann::json models();                                         // GET /api/models

    /// Raw-bytes GET (Phase 34: attachment/image download). Throws on
    /// transport or HTTP error. Path may contain a query string.
    std::string download(const std::string& path);

    /// Minimal percent-encoding for query parameter values.
    static std::string url_encode(const std::string& v);

    /// Phase 66c: the user viewed this thread — advance the hub's
    /// reserved "user" read mark. Best-effort: throws like request().
    void mark_thread_read(const std::string& thread_id);

    // Phase 67: user thread groups (folders).
    nlohmann::json list_thread_groups();
    std::string create_thread_group(const std::string& name);
    void rename_thread_group(const std::string& id, const std::string& name);
    void delete_thread_group(const std::string& id);
    /// Empty group_id returns the thread to its smart group.
    void set_thread_group(const std::string& thread_id,
                          const std::string& group_id);

private:
    nlohmann::json request(const std::string& method, const std::string& path,
                           const nlohmann::json* body = nullptr);
    std::string base_;
    std::string api_key_;
};

} // namespace hive
