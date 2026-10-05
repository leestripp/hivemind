// ApiClient — thin synchronous JSON client for the hub API, ported from
// the Android ApiClient.kt and using the same OmniNet HTTP idiom as
// hive-client. All calls are blocking; call from worker threads only.
#pragma once

#include <AgentStateAnnouncer.h>

#include <nlohmann/json.hpp>

#include <string>
#include <vector>

struct AgentInfo {
    std::string id;
    std::string name;
    bool online{false};
    bool busy{false};
};

struct ThreadMessage {
    std::string id;
    std::string senderKind;  // "user" | "agent"
    std::string senderId;
    std::string senderName;
    std::string body;
    int64_t createdAt{0};
};

struct ThreadActivity {
    bool busy{false};
    bool cancelling{false};
    std::string partial;
};

class ApiClient {
public:
    void setBase(std::string base) { base_ = std::move(base); }
    void setApiKey(std::string key) { api_key_ = std::move(key); }

    std::vector<AgentInfo> listAgents();
    // Create (or reuse) the user's DM thread with an agent; returns thread id.
    std::string resolveThread(const std::string& agent_id);
    std::vector<ThreadMessage> messagesAfter(const std::string& thread_id,
                                             const std::string& after_id);
    ThreadActivity activity(const std::string& thread_id);
    std::string postMessage(const std::string& thread_id,
                            const std::string& body);
    void cancelThread(const std::string& thread_id);
    void cancelAgent(const std::string& agent_id);
    void markRead(const std::string& thread_id);
    // POST /api/notifications/relay convenience for testing.
    void notify(const std::string& title, const std::string& text);

    std::string lastError() const { return last_error_; }

private:
    nlohmann::json request(const std::string& method, const std::string& path,
                           const nlohmann::json* body);

    std::string base_{"http://127.0.0.1:8090"};
    std::string api_key_;
    std::string last_error_;
};
