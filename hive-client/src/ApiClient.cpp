#include "ApiClient.h"
#include "text_utils.h"

#include <OmniNet.h>

#include <stdexcept>
#include <cstdio>

namespace hive {

nlohmann::json ApiClient::request(const std::string& method, const std::string& path,
                                  const nlohmann::json* body) {
    using namespace OmniNet::HTTP;
    ClientConfig cfg;
    cfg.connect_timeout = std::chrono::seconds(5);
    cfg.request_timeout = std::chrono::seconds(30);

    ClientRequest req;
    req.method(method).url(base_ + path);
    if (!api_key_.empty())
        req.header("Authorization", "Bearer " + api_key_);
    if (body) {
        req.header("Content-Type", "application/json");
        req.body(body->dump());
    } else if (method == "POST" || method == "PATCH") {
        req.header("Content-Type", "application/json");
        req.body("{}");
    }

    auto result = Client::get_instance().send_blocking(req, cfg);
    if (!result.has_value())
        throw std::runtime_error(std::string{"request failed: "} +
                                 clientErrorName(result.error()));

    const int http_code = result->statusCode();
    const std::string response = result->bodyAsString();
    nlohmann::json j;
    try { j = nlohmann::json::parse(response); }
    catch (...) {
        throw std::runtime_error("bad JSON (HTTP " +
                                 std::to_string(http_code) + "): " +
                                 response.substr(0, 200));
    }
    if (http_code >= 400) {
        std::string msg = "HTTP " + std::to_string(http_code);
        if (j.contains("error") && j["error"].is_string())
            msg = j["error"].get<std::string>();
        // Phase 19: carry the server's request id so users can quote it.
        if (j.contains("request_id") && j["request_id"].is_string()) {
            const std::string rid = j["request_id"].get<std::string>();
            if (!rid.empty()) msg += "  [" + rid + "]";
        }
        throw std::runtime_error(msg);
    }
    return j;
}

// ── roster ──────────────────────────────────────────────────────────────────

nlohmann::json ApiClient::list_agents() { return request("GET", "/api/agents"); }

nlohmann::json ApiClient::create_agent(const std::string& name,
                                       const std::string& role,
                                       const std::string& model,
                                       const std::string& workspace_mode,
                                       bool require_approval,
                                       const std::string& avatar,
                                       const std::string& worker,
                                       const std::string& type) {
    nlohmann::json body{{"name", name}, {"role", role}, {"model", model},
                        {"workspace_mode", workspace_mode},
                        {"require_approval", require_approval},
                        {"avatar", avatar}};
    if (!worker.empty()) body["worker"] = worker;   // v2: must be an enrolled worker (hub 400s otherwise)
    if (!type.empty())   body["type"]   = type;     // Phase 35
    return request("POST", "/api/agents", &body);
}

nlohmann::json ApiClient::update_agent(const std::string& id,
                                       const std::string& name,
                                       const std::string& role,
                                       const std::string& model,
                                       const std::string& workspace_mode,
                                       bool require_approval,
                                       const std::string& avatar,
                                       const std::string& worker,
                                       const std::string& type) {
    nlohmann::json body{{"name", name}, {"role", role}, {"model", model},
                        {"workspace_mode", workspace_mode},
                        {"require_approval", require_approval},
                        {"avatar", avatar},
                        {"worker", worker}};   // server: "" = keep current
    if (!type.empty()) body["type"] = type;    // Phase 35: "" keep, "default" clears
    return request("PATCH", "/api/agents/" + id, &body);
}

nlohmann::json ApiClient::list_workers() { return request("GET", "/api/workers"); }

void ApiClient::mark_thread_read(const std::string& thread_id) {
    request("POST", "/api/threads/" + thread_id + "/read");
}

nlohmann::json ApiClient::delete_agent(const std::string& id,
                                       const std::string& reassign_to) {
    std::string path = "/api/agents/" + id;
    if (!reassign_to.empty())
        path += "?reassign_to=" + text::url_escape(reassign_to);
    return request("DELETE", path);
}

nlohmann::json ApiClient::list_routines() {
    return request("GET", "/api/routines");
}

nlohmann::json ApiClient::list_global_skills() {
    return request("GET", "/api/skills");
}

void ApiClient::create_skill(const std::string& name,
                             const std::string& description,
                             const std::string& content) {
    nlohmann::json body{{"name", name}, {"description", description},
                        {"content", content}};
    request("POST", "/api/skills", &body);
}

void ApiClient::delete_skill(const std::string& name) {
    request("DELETE", "/api/skills/" + name);
}

nlohmann::json ApiClient::agent_global_skills(const std::string& agent_id) {
    return request("GET", "/api/agents/" + agent_id + "/global-skills");
}

nlohmann::json ApiClient::set_global_skill(const std::string& agent_id,
                                           const std::string& name, bool enabled) {
    nlohmann::json body{{"name", name}, {"enabled", enabled}};
    return request("POST", "/api/agents/" + agent_id + "/global-skills", &body);
}

nlohmann::json ApiClient::list_agent_skills(const std::string& agent_id) {
    return request("GET", "/api/agents/" + agent_id + "/skills");
}

nlohmann::json ApiClient::create_routine(const std::string& agent_id,
                                         const std::string& name,
                                         const std::string& prompt,
                                         const nlohmann::json& schedule) {
    nlohmann::json body{{"agent_id", agent_id}, {"name", name},
                        {"prompt", prompt}, {"schedule", schedule}};
    return request("POST", "/api/routines", &body);
}




nlohmann::json ApiClient::agent_start(const std::string& agent_id) {
    return request("POST", "/api/agents/" + agent_id + "/start");
}

nlohmann::json ApiClient::agent_stop(const std::string& agent_id) {
    return request("POST", "/api/agents/" + agent_id + "/stop");
}

nlohmann::json ApiClient::screen_start(const std::string& agent_id) {
    return request("POST", "/api/agents/" + agent_id + "/screen/start");
}

nlohmann::json ApiClient::screen_status(const std::string& agent_id) {
    return request("GET", "/api/agents/" + agent_id + "/screen");
}

nlohmann::json ApiClient::exec_log(const std::string& agent_id, int limit) {
    return request("GET", "/api/agents/" + agent_id +
                          "/exec-log?limit=" + std::to_string(limit));
}

nlohmann::json ApiClient::update_routine(const std::string& id,
                                         const std::string& name,
                                         const std::string& prompt,
                                         const nlohmann::json& schedule) {
    nlohmann::json body{{"name", name}, {"prompt", prompt},
                        {"schedule", schedule}};
    return request("PATCH", "/api/routines/" + id, &body);
}

void ApiClient::delete_routine(const std::string& id) {
    request("DELETE", "/api/routines/" + id);
}

void ApiClient::run_routine(const std::string& id) {
    request("POST", "/api/routines/" + id + "/run");
}

void ApiClient::set_routine_enabled(const std::string& id, bool enabled) {
    nlohmann::json body{{"enabled", enabled}};
    request("POST", "/api/routines/" + id + "/enabled", &body);
}

// ── threads / chat ──────────────────────────────────────────────────────────

nlohmann::json ApiClient::templates() {
    return request("GET", "/api/templates");
}

nlohmann::json ApiClient::list_threads() { return request("GET", "/api/threads"); }

nlohmann::json ApiClient::create_thread(const std::string& title,
                                        const std::vector<std::string>& agent_ids,
                                        bool reuse_dm) {
    nlohmann::json body{{"title", title}, {"agent_ids", agent_ids},
                        {"reuse_dm", reuse_dm}};
    return request("POST", "/api/threads", &body);
}

nlohmann::json ApiClient::list_messages(const std::string& thread_id,
                                        const std::string& after_id) {
    std::string path = "/api/threads/" + thread_id + "/messages";
    if (!after_id.empty()) path += "?after=" + after_id;
    return request("GET", path);
}

nlohmann::json ApiClient::list_messages_before(
        const std::string& thread_id, const std::string& before_id) {
    // Phase 55: page back through history (server-default window).
    return request("GET", "/api/threads/" + thread_id +
                              "/messages?before=" + before_id);
}

nlohmann::json ApiClient::post_message(const std::string& thread_id,
                                       const std::string& body_text,
                                       const nlohmann::json& attachments) {
    nlohmann::json body{{"body", body_text}};
    if (!attachments.empty()) body["attachments"] = attachments;
    return request("POST", "/api/threads/" + thread_id + "/messages", &body);
}

nlohmann::json ApiClient::upload_attachment(const std::string& thread_id,
                                            const std::string& filename,
                                            const std::string& bytes) {
    using namespace OmniNet::HTTP;
    ClientConfig cfg;
    cfg.connect_timeout = std::chrono::seconds(5);
    cfg.request_timeout = std::chrono::seconds(60);

    ClientRequest req;
    req.method("POST")
       .url(base_ + "/api/threads/" + thread_id +
            "/attachments?name=" + text::url_escape(filename));
    if (!api_key_.empty())
        req.header("Authorization", "Bearer " + api_key_);
    req.header("Content-Type", "application/octet-stream");
    req.body(bytes);

    // Mirror the hub's kMaxUpload (FileRoutes.cpp) — fail fast with a
    // clear message instead of shipping 10 MB+ into a proxy rejection.
    if (bytes.size() > 10 * 1024 * 1024)
        throw std::runtime_error("file too large (10 MB max): " + filename);

    auto result = Client::get_instance().send_blocking(req, cfg);
    if (!result.has_value())
        throw std::runtime_error(std::string{"upload failed: "} +
                                 clientErrorName(result.error()));
    const int http_code = result->statusCode();
    const std::string response = result->bodyAsString();
    if (http_code < 200 || http_code >= 300) {
        // A proxy (nginx) answers with an HTML error page, not the hub's
        // JSON — don't dump markup into the status bar, and name the
        // likely cause for the two size-related codes.
        if (response.find("<html") != std::string::npos ||
            response.find("<!DOCTYPE") != std::string::npos) {
            std::string msg = "upload rejected by proxy: HTTP " +
                              std::to_string(http_code);
            // Only 413 is certainly the size limit — a 400 here once sent
            // us chasing client_max_body_size for hours when the real bug
            // was a locale-corrupted Content-Length (omninet bc2adc4).
            if (http_code == 413)
                msg += " — file over the proxy's size limit "
                       "(nginx client_max_body_size)";
            throw std::runtime_error(msg);
        }
        throw std::runtime_error("upload failed: HTTP " +
                                 std::to_string(http_code) + " " + response);
    }
    try { return nlohmann::json::parse(response); }
    catch (...) {
        throw std::runtime_error("upload: bad JSON in response: " +
                                 response.substr(0, 200));
    }
}

nlohmann::json ApiClient::activity(const std::string& thread_id) {
    return request("GET", "/api/threads/" + thread_id + "/activity");
}

// ── approvals ───────────────────────────────────────────────────────────────

nlohmann::json ApiClient::list_approvals(const std::string& status) {
    return request("GET", "/api/approvals?status=" + status);
}

nlohmann::json ApiClient::resolve_approval(const std::string& id, bool approve) {
    return request("POST", "/api/approvals/" + id +
                   (approve ? "/approve" : "/deny"), nullptr);
}

// ── misc ────────────────────────────────────────────────────────────────────

nlohmann::json ApiClient::models() { return request("GET", "/api/models"); }



nlohmann::json ApiClient::cancel_agent(const std::string& agent_id) {
    // Phase 57: cancel the agent's live turn(s), any thread.
    return request("POST", "/api/agents/" + agent_id + "/cancel");
}

void ApiClient::cancel_thread(const std::string& thread_id) {
    request("POST", "/api/threads/" + thread_id + "/cancel");
}

void ApiClient::delete_thread(const std::string& thread_id) {
    request("DELETE", "/api/threads/" + thread_id);
}

void ApiClient::clear_thread(const std::string& thread_id) {
    // Phase 56: wipe the thread's messages, keep the thread.
    request("POST", "/api/threads/" + thread_id + "/clear");
}

} // namespace hive

namespace hive {

std::string ApiClient::url_encode(const std::string& v) {
    std::string out;
    for (unsigned char c : v) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' ||
            c == '.' || c == '/' )
            out += (char)c;
        else {
            char buf[4];
            std::snprintf(buf, sizeof buf, "%%%02X", c);
            out += buf;
        }
    }
    return out;
}

std::string ApiClient::download(const std::string& path) {
    using namespace OmniNet::HTTP;
    ClientConfig cfg;
    cfg.connect_timeout = std::chrono::seconds(5);
    cfg.request_timeout = std::chrono::seconds(60);   // images can be big

    ClientRequest req;
    req.method("GET").url(base_ + path);
    if (!api_key_.empty())
        req.header("Authorization", "Bearer " + api_key_);

    auto result = Client::get_instance().send_blocking(req, cfg);
    if (!result.has_value())
        throw std::runtime_error(std::string{"download failed: "} +
                                 clientErrorName(result.error()));
    if (result->statusCode() >= 400)
        throw std::runtime_error("download HTTP " +
                                 std::to_string(result->statusCode()));
    return result->bodyAsString();
}

} // namespace hive
