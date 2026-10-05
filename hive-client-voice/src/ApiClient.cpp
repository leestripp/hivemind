#include "ApiClient.h"

#include <OmniNet.h>

#include <chrono>
#include <stdexcept>

namespace {
std::string jstr(const nlohmann::json& j, const char* k) {
    return (j.contains(k) && j[k].is_string()) ? j[k].get<std::string>() : "";
}
}  // namespace

nlohmann::json ApiClient::request(const std::string& method,
                                  const std::string& path,
                                  const nlohmann::json* body) {
    using namespace OmniNet::HTTP;
    ClientConfig cfg;
    cfg.connect_timeout = std::chrono::seconds(5);
    cfg.request_timeout = std::chrono::seconds(30);
    ClientRequest req;
    req.method(method).url(base_ + path);
    if (!api_key_.empty()) req.header("Authorization", "Bearer " + api_key_);
    if (body) {
        req.header("Content-Type", "application/json");
        req.body(body->dump());
    } else if (method == "POST" || method == "PATCH") {
        req.header("Content-Type", "application/json");
        req.body("{}");
    }
    auto result = Client::get_instance().send_blocking(req, cfg);
    if (!result.has_value()) {
        last_error_ = std::string{"request failed: "} + clientErrorName(result.error());
        throw std::runtime_error(last_error_);
    }
    const int code = result->statusCode();
    nlohmann::json j;
    try { j = nlohmann::json::parse(result->bodyAsString()); }
    catch (...) { j = nlohmann::json::object(); }
    if (code >= 400) {
        std::string msg = "HTTP " + std::to_string(code);
        if (j.contains("error") && j["error"].is_string())
            msg = j["error"].get<std::string>();
        last_error_ = msg;
        throw std::runtime_error(msg);
    }
    return j;
}

std::vector<AgentInfo> ApiClient::listAgents() {
    auto j = request("GET", "/api/agents", nullptr);
    std::vector<AgentInfo> out;
    if (!j.is_array()) return out;
    for (const auto& a : j) {
        AgentInfo i{jstr(a, "id"), jstr(a, "name"), a.value("online", false),
                    a.value("busy", false)};
        if (!i.id.empty()) out.push_back(std::move(i));
    }
    return out;
}

std::string ApiClient::resolveThread(const std::string& agent_id) {
    nlohmann::json body = {{"agent_id", agent_id}};
    return jstr(request("POST", "/api/threads", &body), "id");
}

std::vector<ThreadMessage> ApiClient::messagesAfter(const std::string& thread_id,
                                                    const std::string& after_id) {
    std::string path = "/api/threads/" + thread_id + "/messages";
    if (!after_id.empty()) path += "?after=" + after_id;
    auto j = request("GET", path, nullptr);
    std::vector<ThreadMessage> out;
    if (!j.is_array()) return out;
    for (const auto& m : j) {
        ThreadMessage msg;
        msg.id = jstr(m, "id");
        msg.senderKind = jstr(m, "sender_kind");
        msg.senderId = jstr(m, "sender_id");
        msg.senderName = jstr(m, "sender_name");
        msg.body = jstr(m, "body");
        msg.createdAt = m.value("created_at", int64_t{0});
        out.push_back(std::move(msg));
    }
    return out;
}

ThreadActivity ApiClient::activity(const std::string& thread_id) {
    auto j = request("GET", "/api/threads/" + thread_id + "/activity", nullptr);
    return {j.value("busy", false), j.value("cancelling", false),
            jstr(j, "partial")};
}

std::string ApiClient::postMessage(const std::string& thread_id,
                                   const std::string& body) {
    nlohmann::json b = {{"body", body}};
    return jstr(request("POST", "/api/threads/" + thread_id + "/messages", &b), "id");
}

void ApiClient::cancelThread(const std::string& thread_id) {
    request("POST", "/api/threads/" + thread_id + "/cancel", nullptr);
}

void ApiClient::cancelAgent(const std::string& agent_id) {
    request("POST", "/api/agents/" + agent_id + "/cancel", nullptr);
}

void ApiClient::markRead(const std::string& thread_id) {
    request("POST", "/api/threads/" + thread_id + "/read", nullptr);
}

void ApiClient::notify(const std::string& title, const std::string& text) {
    nlohmann::json b = {{"title", title}, {"text", text}};
    request("POST", "/api/notifications/relay", &b);
}
