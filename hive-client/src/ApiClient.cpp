#include "ApiClient.h"

#include <curl/curl.h>

#include <stdexcept>

namespace hive {
namespace {

size_t write_cb(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* out = static_cast<std::string*>(userdata);
    out->append(ptr, size * nmemb);
    return size * nmemb;
}

} // namespace

nlohmann::json ApiClient::request(const std::string& method, const std::string& path,
                                  const nlohmann::json* body) {
    CURL* curl = curl_easy_init();
    if (!curl) throw std::runtime_error("curl init failed");

    std::string url = base_ + path;
    std::string response;
    std::string payload;
    struct curl_slist* headers = nullptr;
    if (!api_key_.empty()) {
        std::string auth = "Authorization: Bearer " + api_key_;
        headers = curl_slist_append(headers, auth.c_str());
    }

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5L);

    if (method == "POST" || method == "PATCH" || method == "DELETE") {
        headers = curl_slist_append(headers, "Content-Type: application/json");
        if (body) payload = body->dump();
        curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, method.c_str());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, payload.c_str());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)payload.size());
    }
    if (headers) curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

    CURLcode rc = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    if (headers) curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (rc != CURLE_OK)
        throw std::runtime_error(std::string{"request failed: "} +
                                 curl_easy_strerror(rc));

    nlohmann::json j;
    try { j = nlohmann::json::parse(response); }
    catch (...) {
        throw std::runtime_error("bad JSON (HTTP " +
                                 std::to_string(http_code) + "): " +
                                 response.substr(0, 200));
    }
    if (http_code >= 400) {
        std::string msg = j.value("error", "HTTP " + std::to_string(http_code));
        // Phase 19: carry the server's request id so users can quote it.
        const std::string rid = j.value("request_id", "");
        if (!rid.empty()) msg += "  [" + rid + "]";
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
                                       const std::string& avatar) {
    nlohmann::json body{{"name", name}, {"role", role}, {"model", model},
                        {"workspace_mode", workspace_mode},
                        {"require_approval", require_approval},
                        {"avatar", avatar}};
    return request("POST", "/api/agents", &body);
}

nlohmann::json ApiClient::update_agent(const std::string& id,
                                       const std::string& name,
                                       const std::string& role,
                                       const std::string& model,
                                       const std::string& workspace_mode,
                                       bool require_approval,
                                       const std::string& avatar) {
    nlohmann::json body{{"name", name}, {"role", role}, {"model", model},
                        {"workspace_mode", workspace_mode},
                        {"require_approval", require_approval},
                        {"avatar", avatar}};
    return request("PATCH", "/api/agents/" + id, &body);
}

nlohmann::json ApiClient::delete_agent(const std::string& id,
                                       const std::string& reassign_to) {
    std::string path = "/api/agents/" + id;
    if (!reassign_to.empty()) path += "?reassign_to=" + reassign_to;
    return request("DELETE", path);
}

nlohmann::json ApiClient::list_routines() {
    return request("GET", "/api/routines");
}

nlohmann::json ApiClient::list_global_skills() {
    return request("GET", "/api/skills");
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

nlohmann::json ApiClient::container_status(const std::string& agent_id) {
    return request("GET", "/api/agents/" + agent_id + "/container/status");
}

nlohmann::json ApiClient::container_start(const std::string& agent_id) {
    return request("POST", "/api/agents/" + agent_id + "/container/start");
}

nlohmann::json ApiClient::container_stop(const std::string& agent_id) {
    return request("POST", "/api/agents/" + agent_id + "/container/stop");
}

nlohmann::json ApiClient::screen_start(const std::string& agent_id) {
    return request("POST", "/api/agents/" + agent_id + "/screen/start");
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
    CURL* curl = curl_easy_init();
    if (!curl) throw std::runtime_error("curl init failed");

    char* esc = curl_easy_escape(curl, filename.c_str(), 0);
    std::string url = base_ + "/api/threads/" + thread_id +
                      "/attachments?name=" + (esc ? esc : "file");
    if (esc) curl_free(esc);

    std::string response;
    struct curl_slist* headers = nullptr;
    if (!api_key_.empty()) {
        std::string auth = "Authorization: Bearer " + api_key_;
        headers = curl_slist_append(headers, auth.c_str());
    }
    headers = curl_slist_append(headers,
                                "Content-Type: application/octet-stream");
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 60L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, bytes.data());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)bytes.size());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

    CURLcode rc = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (rc != CURLE_OK)
        throw std::runtime_error(std::string{"upload failed: "} +
                                 curl_easy_strerror(rc));
    if (http_code < 200 || http_code >= 300)
        throw std::runtime_error("upload failed: HTTP " +
                                 std::to_string(http_code) + " " + response);
    return nlohmann::json::parse(response);
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
nlohmann::json ApiClient::providers() { return request("GET", "/api/providers"); }

bool ApiClient::healthy() {
    try { return request("GET", "/api/health").value("status", "") == "ok"; }
    catch (...) { return false; }
}


void ApiClient::cancel_thread(const std::string& thread_id) {
    request("POST", "/api/threads/" + thread_id + "/cancel");
}

void ApiClient::delete_thread(const std::string& thread_id) {
    request("DELETE", "/api/threads/" + thread_id);
}

} // namespace hive
