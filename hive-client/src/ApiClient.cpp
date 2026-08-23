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
                                       bool require_approval) {
    nlohmann::json body{{"name", name}, {"role", role}, {"model", model},
                        {"workspace_mode", workspace_mode},
                        {"require_approval", require_approval}};
    return request("POST", "/api/agents", &body);
}

nlohmann::json ApiClient::container_status(const std::string& agent_id) {
    return request("GET", "/api/agents/" + agent_id + "/container/status");
}

// ── threads / chat ──────────────────────────────────────────────────────────

nlohmann::json ApiClient::list_threads() { return request("GET", "/api/threads"); }

nlohmann::json ApiClient::create_thread(const std::string& title,
                                        const std::vector<std::string>& agent_ids) {
    nlohmann::json body{{"title", title}, {"agent_ids", agent_ids}};
    return request("POST", "/api/threads", &body);
}

nlohmann::json ApiClient::list_messages(const std::string& thread_id,
                                        const std::string& after_id) {
    std::string path = "/api/threads/" + thread_id + "/messages";
    if (!after_id.empty()) path += "?after=" + after_id;
    return request("GET", path);
}

nlohmann::json ApiClient::post_message(const std::string& thread_id,
                                       const std::string& body_text) {
    nlohmann::json body{{"body", body_text}};
    return request("POST", "/api/threads/" + thread_id + "/messages", &body);
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

bool ApiClient::healthy() {
    try { return request("GET", "/api/health").value("status", "") == "ok"; }
    catch (...) { return false; }
}

} // namespace hive
