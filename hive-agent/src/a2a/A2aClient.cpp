// SPDX-License-Identifier: BSL-1.0
// src/library/a2a/A2aClient.cpp
//
// A2A HTTP+JSON/REST client implementation backed by OmniNet via the
// internal HttpSession adapter and the public SseParser helper.

#include "a2a/A2aClient.h"

#include <cctype>
#include <chrono>
#include <expected>
#include <sstream>
#include <string_view>

#include "api/HttpSession.h"
#include "core/Logger.h"
#include "a2a/a2a_json.h"
#include "net/SseParser.h"

namespace hive::a2a {

namespace {

using json = nlohmann::json;
using namespace std::string_view_literals;
using namespace hive::a2a::detail;

// ============================================================================
// URL / query helpers
// ============================================================================

[[nodiscard]] std::string normalize_base_url(std::string url) {
    while (!url.empty() && url.back() == '/') url.pop_back();
    return url;
}

[[nodiscard]] std::string join_url(const std::string& base, std::string_view path) {
    if (base.empty()) return std::string(path);
    if (path.empty()) return base;
    if (path.front() == '/') return base + std::string(path);
    return base + "/" + std::string(path);
}

[[nodiscard]] std::string url_encode(std::string_view s) {
    std::ostringstream out;
    out.fill('0');
    out << std::hex << std::uppercase;
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out << static_cast<char>(c);
        } else {
            out << '%' << std::setw(2) << static_cast<int>(c);
        }
    }
    return out.str();
}

[[nodiscard]] std::string build_query(
    const std::vector<std::pair<std::string, std::string>>& params) {
    std::ostringstream out;
    bool first = true;
    for (const auto& [k, v] : params) {
        if (!first) out << '&';
        first = false;
        out << url_encode(k) << '=' << url_encode(v);
    }
    return out.str();
}

// ============================================================================
// Enum conversions
// ============================================================================

struct EnumName {
    std::string_view name;
    TaskState state;
};
// Wire format uses ProtoJSON enum names so we interoperate with the
// official A2A SDKs. task_state_from_string() still accepts the old
// lowercase / hyphenated forms for backward compatibility.
constexpr EnumName kTaskStateNames[] = {
    {"TASK_STATE_UNSPECIFIED", TaskState::unspecified},
    {"TASK_STATE_SUBMITTED", TaskState::submitted},
    {"TASK_STATE_WORKING", TaskState::working},
    {"TASK_STATE_COMPLETED", TaskState::completed},
    {"TASK_STATE_FAILED", TaskState::failed},
    {"TASK_STATE_CANCELED", TaskState::canceled},
    {"TASK_STATE_INPUT_REQUIRED", TaskState::input_required},
    {"TASK_STATE_AUTH_REQUIRED", TaskState::auth_required},
    {"TASK_STATE_REJECTED", TaskState::rejected},
};

} // anonymous namespace

std::string to_string(TaskState s) {
    for (const auto& e : kTaskStateNames) {
        if (e.state == s) return std::string(e.name);
    }
    return "TASK_STATE_UNSPECIFIED";
}

TaskState task_state_from_string(std::string_view s) {
    for (const auto& e : kTaskStateNames) {
        if (e.name == s) return e.state;
    }
    // Backward compatibility with the older lowercase / hyphenated wire form.
    if (s == "submitted")    return TaskState::submitted;
    if (s == "working")      return TaskState::working;
    if (s == "completed")    return TaskState::completed;
    if (s == "failed")       return TaskState::failed;
    if (s == "canceled")     return TaskState::canceled;
    if (s == "input-required")  return TaskState::input_required;
    if (s == "auth-required")   return TaskState::auth_required;
    if (s == "rejected")     return TaskState::rejected;
    return TaskState::unspecified;
}

std::string to_string(Role r) {
    switch (r) {
        case Role::user:   return "ROLE_USER";
        case Role::agent:  return "ROLE_AGENT";
        default:           return "ROLE_UNSPECIFIED";
    }
}

Role role_from_string(std::string_view s) {
    if (s == "ROLE_USER")  return Role::user;
    if (s == "ROLE_AGENT") return Role::agent;
    // Backward compatibility with the older lowercase wire form.
    if (s == "user")  return Role::user;
    if (s == "agent") return Role::agent;
    return Role::unspecified;
}

std::string to_string(PartKind k) {
    switch (k) {
        case PartKind::file: return "file";
        case PartKind::data: return "data";
        default:             return "text";
    }
}

PartKind part_kind_from_string(std::string_view s) {
    if (s == "file") return PartKind::file;
    if (s == "data") return PartKind::data;
    return PartKind::text;
}

std::string A2aError::what() const {
    std::string out = "A2A error";
    if (http_status != 0) out += " (HTTP " + std::to_string(http_status) + ")";
    if (!code.empty()) out += " [" + code + "]";
    if (!message.empty()) out += ": " + message;
    return out;
}

// ============================================================================
// Part factories
// ============================================================================

Part Part::from_text(std::string t, std::optional<std::string> media_type) {
    Part p;
    p.kind = PartKind::text;
    p.text = std::move(t);
    p.media_type = std::move(media_type);
    return p;
}

Part Part::from_url(std::string u, std::optional<std::string> filename,
                    std::optional<std::string> media_type) {
    Part p;
    p.kind = PartKind::file;
    p.url = std::move(u);
    p.filename = std::move(filename);
    p.media_type = std::move(media_type);
    return p;
}

Part Part::from_data(nlohmann::json d, std::optional<std::string> media_type) {
    Part p;
    p.kind = PartKind::data;
    p.data = std::move(d);
    p.media_type = std::move(media_type);
    return p;
}

namespace {

// ============================================================================
// HTTP / error helpers
// ============================================================================

[[nodiscard]] std::string extract_reason(const json& details) {
    if (!details.is_array()) return {};
    for (const auto& d : details) {
        if (!d.is_object()) continue;
        auto type = opt_str(d, "@type").value_or("");
        if (type == "type.googleapis.com/google.rpc.ErrorInfo" ||
            type.find("ErrorInfo") != std::string::npos) {
            return opt_str(d, "reason").value_or("");
        }
    }
    return {};
}

[[nodiscard]] A2aError make_error(const net::HttpResponse& r) {
    A2aError err;
    err.http_status = static_cast<int>(r.status_code);

    try {
        auto j = json::parse(r.body);
        auto eit = j.find("error");
        if (eit != j.end() && eit->is_object()) {
            err.message = opt_str(*eit, "message").value_or(r.body);
            err.details = opt_metadata(*eit, "details");
            err.code = extract_reason(err.details);
            if (err.code.empty()) err.code = opt_str(*eit, "status").value_or("");
        } else {
            err.message = r.body;
        }
    } catch (...) {
        err.message = r.body.empty() ? "HTTP error" : r.body;
    }
    if (err.message.empty()) err.message = "HTTP " + std::to_string(r.status_code);
    return err;
}

[[nodiscard]] net::HttpRequest make_request(std::string url,
                                            std::string method,
                                            std::optional<std::string> bearer,
                                            const std::optional<std::string>& tenant,
                                            std::string body = {}) {
    net::HttpRequest req;
    req.url = std::move(url);
    req.method = std::move(method);
    req.body = std::move(body);
    req.bearer_token = bearer.value_or("");
    req.headers.push_back("Content-Type: application/a2a+json");
    req.headers.push_back("Accept: application/a2a+json");
    req.headers.push_back("A2A-Version: 1.0");
    if (tenant && !tenant->empty()) {
        req.headers.push_back("A2A-Tenant: " + *tenant);
    }
    req.timeout_seconds = 300;
    return req;
}

[[nodiscard]] std::expected<net::HttpResponse, A2aError>
submit_sync(const net::HttpRequest& req) {
    auto fut = net::HttpSession::get_instance().submit(req);
    auto result = fut.get();
    if (!result) {
        A2aError err;
        err.message = net::to_string(result.error());
        return std::unexpected(err);
    }
    if (!result->ok()) {
        return std::unexpected(make_error(*result));
    }
    return *result;
}

} // anonymous namespace

// ============================================================================
// A2aClient public API
// ============================================================================

A2aClient::A2aClient(std::string base_url,
                     std::optional<std::string> bearer_token,
                     std::optional<std::string> tenant)
    : base_url_(normalize_base_url(std::move(base_url)))
    , bearer_token_(std::move(bearer_token))
    , tenant_(std::move(tenant)) {}

std::unique_ptr<A2aClient> create_a2a_client(std::string base_url,
                                             std::optional<std::string> bearer_token,
                                             std::optional<std::string> tenant) {
    return std::make_unique<A2aClient>(std::move(base_url), std::move(bearer_token),
                                       std::move(tenant));
}

A2aResult<AgentCard> A2aClient::get_agent_card() const {
    auto req = make_request(join_url(base_url_, "/.well-known/agent-card.json"),
                            "GET", bearer_token_, tenant_);
    auto res = submit_sync(req);
    if (!res) return std::unexpected(res.error());
    try {
        return parse_agent_card(json::parse(res->body));
    } catch (const std::exception& e) {
        LOG_ERROR(std::string("A2A get_agent_card JSON parse failed: ") + e.what());
        A2aError err;
        err.message = std::string("invalid JSON: ") + e.what();
        return std::unexpected(err);
    }
}

A2aResult<SendMessageResult> A2aClient::send_message(
    const SendMessageRequest& request) const {
    auto req = make_request(join_url(base_url_, "/v1/message:send"), "POST",
                            bearer_token_, tenant_, to_json(request).dump());
    auto res = submit_sync(req);
    if (!res) return std::unexpected(res.error());
    try {
        return parse_send_message_response(json::parse(res->body));
    } catch (const std::exception& e) {
        LOG_ERROR(std::string("A2A send_message JSON parse failed: ") + e.what());
        A2aError err;
        err.message = std::string("invalid JSON: ") + e.what();
        return std::unexpected(err);
    }
}

A2aResult<Task> A2aClient::get_task(const GetTaskRequest& request) const {
    std::vector<std::pair<std::string, std::string>> params;
    if (request.tenant && !request.tenant->empty()) params.emplace_back("tenant", *request.tenant);
    if (request.history_length) params.emplace_back("historyLength", std::to_string(*request.history_length));

    std::string url = join_url(base_url_, "/v1/tasks/" + request.id);
    auto q = build_query(params);
    if (!q.empty()) url += "?" + q;

    auto req = make_request(url, "GET", bearer_token_, tenant_);
    auto res = submit_sync(req);
    if (!res) return std::unexpected(res.error());
    try {
        return parse_task(json::parse(res->body));
    } catch (const std::exception& e) {
        LOG_ERROR(std::string("A2A get_task JSON parse failed: ") + e.what());
        A2aError err;
        err.message = std::string("invalid JSON: ") + e.what();
        return std::unexpected(err);
    }
}

A2aResult<ListTasksResponse> A2aClient::list_tasks(
    const ListTasksRequest& request) const {
    std::vector<std::pair<std::string, std::string>> params;
    if (request.tenant && !request.tenant->empty()) params.emplace_back("tenant", *request.tenant);
    if (request.context_id) params.emplace_back("contextId", *request.context_id);
    if (request.status) params.emplace_back("status", to_string(*request.status));
    if (request.page_size) params.emplace_back("pageSize", std::to_string(*request.page_size));
    if (request.page_token) params.emplace_back("pageToken", *request.page_token);
    if (request.history_length) params.emplace_back("historyLength", std::to_string(*request.history_length));
    if (request.status_timestamp_after) params.emplace_back("statusTimestampAfter", *request.status_timestamp_after);
    if (request.include_artifacts) params.emplace_back("includeArtifacts", *request.include_artifacts ? "true" : "false");

    std::string url = join_url(base_url_, "/v1/tasks");
    auto q = build_query(params);
    if (!q.empty()) url += "?" + q;

    auto req = make_request(url, "GET", bearer_token_, tenant_);
    auto res = submit_sync(req);
    if (!res) return std::unexpected(res.error());
    try {
        return parse_list_tasks_response(json::parse(res->body));
    } catch (const std::exception& e) {
        LOG_ERROR(std::string("A2A list_tasks JSON parse failed: ") + e.what());
        A2aError err;
        err.message = std::string("invalid JSON: ") + e.what();
        return std::unexpected(err);
    }
}

A2aResult<Task> A2aClient::cancel_task(const CancelTaskRequest& request) const {
    auto req = make_request(join_url(base_url_, "/v1/tasks/" + request.id + ":cancel"),
                            "POST", bearer_token_, tenant_, to_json(request).dump());
    auto res = submit_sync(req);
    if (!res) return std::unexpected(res.error());
    try {
        return parse_task(json::parse(res->body));
    } catch (const std::exception& e) {
        LOG_ERROR(std::string("A2A cancel_task JSON parse failed: ") + e.what());
        A2aError err;
        err.message = std::string("invalid JSON: ") + e.what();
        return std::unexpected(err);
    }
}

A2aResult<void> A2aClient::send_streaming_message(
    const SendMessageRequest& request,
    IA2aEventHandler& handler) const {
    net::HttpRequest req = make_request(join_url(base_url_, "/v1/message:stream"),
                                        "POST", bearer_token_, tenant_,
                                        to_json(request).dump());
    net::SseParser parser;
    req.stream_callback = [&parser, &handler](std::string_view chunk) {
        parser.feed(chunk);
        while (auto ev = parser.try_pop()) {
            try {
                auto j = json::parse(ev->data);
                dispatch_stream_response(j, handler);
            } catch (const std::exception& e) {
                A2aError err;
                err.message = std::string("stream JSON parse failed: ") + e.what();
                handler.on_error(err);
            }
        }
    };

    auto fut = net::HttpSession::get_instance().submit(req);
    auto result = fut.get();
    parser.finish();
    while (auto ev = parser.try_pop()) {
        try {
            auto j = json::parse(ev->data);
            dispatch_stream_response(j, handler);
        } catch (const std::exception& e) {
            A2aError err;
            err.message = std::string("stream JSON parse failed: ") + e.what();
            handler.on_error(err);
        }
    }
    if (!result) {
        return std::unexpected(A2aError{0, "", net::to_string(result.error()), nullptr});
    }
    if (!result->ok()) {
        return std::unexpected(make_error(*result));
    }
    handler.on_complete();
    return {};
}

A2aResult<void> A2aClient::subscribe_to_task(
    const SubscribeToTaskRequest& request,
    IA2aEventHandler& handler) const {
    std::vector<std::pair<std::string, std::string>> params;
    if (request.tenant && !request.tenant->empty()) params.emplace_back("tenant", *request.tenant);

    std::string url = join_url(base_url_, "/v1/tasks/" + request.id + ":subscribe");
    auto q = build_query(params);
    if (!q.empty()) url += "?" + q;

    net::HttpRequest req = make_request(url, "GET", bearer_token_, tenant_);
    net::SseParser parser;
    req.stream_callback = [&parser, &handler](std::string_view chunk) {
        parser.feed(chunk);
        while (auto ev = parser.try_pop()) {
            try {
                auto j = json::parse(ev->data);
                dispatch_stream_response(j, handler);
            } catch (const std::exception& e) {
                A2aError err;
                err.message = std::string("stream JSON parse failed: ") + e.what();
                handler.on_error(err);
            }
        }
    };

    auto fut = net::HttpSession::get_instance().submit(req);
    auto result = fut.get();
    parser.finish();
    while (auto ev = parser.try_pop()) {
        try {
            auto j = json::parse(ev->data);
            dispatch_stream_response(j, handler);
        } catch (const std::exception& e) {
            A2aError err;
            err.message = std::string("stream JSON parse failed: ") + e.what();
            handler.on_error(err);
        }
    }
    if (!result) {
        return std::unexpected(A2aError{0, "", net::to_string(result.error()), nullptr});
    }
    if (!result->ok()) {
        return std::unexpected(make_error(*result));
    }
    handler.on_complete();
    return {};
}

} // namespace hive::a2a
