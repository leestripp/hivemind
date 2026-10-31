#include "ApiClient.h"

#include <cstdio>

#include <OmniNet.h>

#include <chrono>

namespace hv {

namespace {
// "https://leestrippai.com" -> "leestrippai.com" (port kept if present)
std::string hostOf(const std::string& base) {
    const auto s = base.find("://");
    const auto from = (s == std::string::npos) ? 0 : s + 3;
    const auto e = base.find('/', from);
    return base.substr(from, e == std::string::npos ? e : e - from);
}
}  // namespace


namespace {
std::string jstr(const nlohmann::json& j, const char* k) {
    return (j.contains(k) && j[k].is_string()) ? j[k].get<std::string>() : "";
}

const nlohmann::json& envelope(const nlohmann::json& j, const char* key) {
    static const nlohmann::json kEmpty = nlohmann::json::array();
    return (j.contains(key) && j[key].is_array()) ? j[key] : kEmpty;
}
}  // namespace

nlohmann::json ApiClient::request(const std::string& method,
                                  const std::string& path,
                                  const nlohmann::json* body,
                                  bool tolerate_error) {
    using namespace OmniNet::HTTP;
    ClientConfig cfg;
    cfg.connect_timeout = std::chrono::seconds(5);
    cfg.request_timeout = std::chrono::seconds(30);
    ClientRequest req;
    req.method(method).url(base_ + path);
    if (!api_key_.empty()) req.header("Authorization", "Bearer " + api_key_);
    req.header("Accept", "application/json");
    if (body) {
        req.header("Content-Type", "application/json");
        req.body(body->dump());
    } else if (method == "POST" || method == "PATCH") {
        req.header("Content-Type", "application/json");
        req.body("{}");
    }
    auto result = Client::get_instance().send_blocking(req, cfg);
    if (!result.has_value()) {
        last_error_ = "request failed: " +
                      std::string{clientErrorName(result.error())} + " (" +
                      hostOf(base_) + ")";
        // Request failures were invisible until they surfaced in the UI —
        // log every one so connection flapping is diagnosable.
        fprintf(stderr, "hive-net: %s %s -> %s\n", method.c_str(),
                path.c_str(), last_error_.c_str());
        if (tolerate_error) return nlohmann::json::object();
        throw std::runtime_error(last_error_);
    }
    const int code = result->statusCode();
    if (code < 200 || code >= 300)
        fprintf(stderr, "hive-net: %s %s -> HTTP %d (%s)\n", method.c_str(),
                path.c_str(), code, hostOf(base_).c_str());
    nlohmann::json j = nlohmann::json::object();
    try {
        j = nlohmann::json::parse(result->bodyAsString());
    } catch (...) {
    }
    if (code < 200 || code >= 300) {
        std::string msg = "HTTP " + std::to_string(code);
        if (j.contains("error") && j["error"].is_string())
            msg = j["error"].get<std::string>();
        last_error_ = msg;
        if (tolerate_error) return j;
        throw ApiException(code, msg);
    }
    return j;
}

std::vector<AgentInfo> ApiClient::listAgents() {
    auto j = request("GET", "/api/agents", nullptr, false);
    std::vector<AgentInfo> out;
    for (const auto& a : envelope(j, "agents")) {
        AgentInfo i;
        i.id = jstr(a, "id");
        i.name = jstr(a, "name");
        i.role = jstr(a, "role");
        i.avatar = jstr(a, "avatar");
        i.state = jstr(a, "state");
        i.worker = jstr(a, "worker");
        i.busy = a.value("busy", false);
        // The roster payload exposes state ("online"/"offline") rather
        // than a boolean — derive it, tolerating both shapes.
        const std::string state = jstr(a, "state");
        i.online = a.value("online", state != "offline");
        if (!i.id.empty()) out.push_back(std::move(i));
    }
    return out;
}

std::vector<WorkerInfo> ApiClient::listWorkers() {
    auto j = request("GET", "/api/workers", nullptr, false);
    std::vector<WorkerInfo> out;
    for (const auto& w : envelope(j, "workers")) {
        WorkerInfo i;
        i.name = jstr(w, "name");
        i.online = w.value("online", false);
        if (!i.name.empty()) out.push_back(std::move(i));
    }
    return out;
}

std::string ApiClient::openDm(const std::string& agent_id) {
    nlohmann::json body = {
        {"agent_ids", nlohmann::json::array({agent_id})},
        {"title", "Voice"},
        {"kind", "dm"},
        {"reuse_dm", true},
    };
    return jstr(request("POST", "/api/threads", &body, false), "id");
}

std::vector<ThreadMessage> ApiClient::messagesAfter(
    const std::string& thread_id, const std::string& after_id) {
    std::string path = "/api/threads/" + thread_id + "/messages";
    if (!after_id.empty()) path += "?after=" + after_id;
    auto j = request("GET", path, nullptr, false);
    std::vector<ThreadMessage> out;
    for (const auto& m : envelope(j, "messages")) {
        ThreadMessage msg;
        msg.id = jstr(m, "id");
        msg.senderKind = jstr(m, "sender_kind");
        msg.senderId = jstr(m, "sender_id");
        msg.senderName = jstr(m, "sender_name");
        msg.body = jstr(m, "body");
        // The hub sends created_at as an ISO-8601 string ("2026-10-05T...")
        // while this field expects epoch millis. Nothing consumes the
        // value yet, so accept either form without throwing — the strict
        // m.value<>(int64_t) here made EVERY poll tick throw
        // type_error.302 once any message existed, which the poll loop
        // counted as a network failure until it announced "connection
        // lost" 45 s later.
        if (const auto it = m.find("created_at");
            it != m.end() && it->is_number())
            msg.createdAt = it->get<int64_t>();
        out.push_back(std::move(msg));
    }
    return out;
}


std::string ApiClient::latestMessageId(const std::string& thread_id) {
    auto j = request("GET", "/api/threads/" + thread_id +
                                "/messages?limit=1",
                     nullptr, false);
    const auto& arr = envelope(j, "messages");
    if (arr.empty()) return "";
    return jstr(arr.back(), "id");
}

ThreadActivity ApiClient::activity(const std::string& thread_id) {
    auto j = request("GET", "/api/threads/" + thread_id + "/activity",
                     nullptr, false);
    ThreadActivity act;
    act.busy = j.value("busy", false);
    for (const auto& t : envelope(j, "turns")) {
        // Done turns linger with cancelling still true — only a LIVE
        // cancelling turn means "dying" (Android comment, verbatim).
        if (t.value("cancelling", false) && !t.value("done", false))
            act.cancelling = true;
        const std::string e = jstr(t, "error");
        if (!e.empty()) {
            act.error = e;
            break;
        }
        if (act.partial.empty()) act.partial = jstr(t, "partial");
    }
    return act;
}

bool ApiClient::agentBusy(const std::string& agent_id) {
    const auto j = request("GET", "/api/agents/activity", nullptr, false);
    const auto it = j.find(agent_id);
    return it != j.end() && it->is_object() && it->value("busy", false);
}

std::string ApiClient::postMessage(const std::string& thread_id,
                                   const std::string& body,
                                   const std::string& source) {
    nlohmann::json b = {{"body", body}, {"source", source}};
    return jstr(request("POST", "/api/threads/" + thread_id + "/messages",
                        &b, false),
                "message_id");
}

void ApiClient::cancelThread(const std::string& thread_id) {
    request("POST", "/api/threads/" + thread_id + "/cancel", nullptr, true);
}

void ApiClient::cancelAgent(const std::string& agent_id) {
    request("POST", "/api/agents/" + agent_id + "/cancel", nullptr, true);
}

void ApiClient::markRead(const std::string& thread_id) {
    request("POST", "/api/threads/" + thread_id + "/read", nullptr, true);
}

}  // namespace hv
