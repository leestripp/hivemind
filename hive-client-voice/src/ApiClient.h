#pragma once

#include <nlohmann/json.hpp>

#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace hv {

// Thrown on any HTTP >= 400; carries the status code so callers can
// special-case 404 thread-vanished (reresolveIfThreadGone in the app).
class ApiException : public std::runtime_error {
 public:
    ApiException(int code, const std::string& message)
        : std::runtime_error("HTTP " + std::to_string(code) + ": " + message),
          code_(code) {}
    int code() const { return code_; }

 private:
    int code_;
};

struct AgentInfo {
    std::string id;
    std::string name;
    std::string role;
    std::string avatar;  // stored choice; "" = name-hash auto (see Avatars.h)
    std::string state;   // fleet heartbeat state ("ready"/"down"/...)
    bool busy = false;
    bool online = false;
};

struct ThreadMessage {
    std::string id;
    std::string senderKind;
    std::string senderId;
    std::string senderName;
    std::string body;
    std::int64_t createdAt = 0;
};

struct ThreadActivity {
    bool busy = false;
    bool cancelling = false;  // a LIVE turn is being cancelled
    std::string error;
    std::string partial;
};

// Minimal REST client for the hive-server hub. Blocking — call from a
// worker thread only. Mirrors the Android ApiClient.kt 1:1 (endpoint
// shapes included: list endpoints return {"agents":[...]}-style
// envelopes, messages return {"messages":[...]}).
class ApiClient {
 public:
    ApiClient(std::string base, std::string apiKey)
        : base_(std::move(base)), api_key_(std::move(apiKey)) {
        while (!base_.empty() && base_.back() == '/') base_.pop_back();
    }

    std::vector<AgentInfo> listAgents();
    // POST /api/threads with reuse_dm — opens (or reopens) the 1:1.
    std::string openDm(const std::string& agent_id);
    std::vector<ThreadMessage> messagesAfter(const std::string& thread_id,
                                             const std::string& after_id);
    std::string latestMessageId(const std::string& thread_id);
    ThreadActivity activity(const std::string& thread_id);
    // Returns the new message id. source="voice" lets the hub stamp the
    // channel server-side (2026-10-04 Plan.md §3.4).
    std::string postMessage(const std::string& thread_id,
                            const std::string& body,
                            const std::string& source = "user");
    void cancelThread(const std::string& thread_id);  // best effort
    void cancelAgent(const std::string& agent_id);    // best effort
    void markRead(const std::string& thread_id);

    const std::string& lastError() const { return last_error_; }

 private:
    nlohmann::json request(const std::string& method, const std::string& path,
                           const nlohmann::json* body, bool tolerate_error);

    std::string base_;
    std::string api_key_;
    std::string last_error_;
};

}  // namespace hv
