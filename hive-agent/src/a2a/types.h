// SPDX-License-Identifier: BSL-1.0
// include/hive-brain/a2a/types.h
//
// Public data types for the Agent2Agent (A2A) Protocol client.
//
// These structures mirror the canonical protobuf definitions in the A2A
// specification (https://a2a-protocol.org/latest/specification/). JSON
// wire serialization uses ProtoJSON-style camelCase field names; the C++
// API uses snake_case for readability.

#pragma once

#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

namespace hive::a2a {

// ============================================================================
// Enums
// ============================================================================

enum class TaskState {
    unspecified,
    submitted,
    working,
    completed,
    failed,
    canceled,
    input_required,
    auth_required,
    rejected,
};

[[nodiscard]] std::string to_string(TaskState s);
[[nodiscard]] TaskState task_state_from_string(std::string_view s);

enum class Role {
    unspecified,
    user,
    agent,
};

[[nodiscard]] std::string to_string(Role r);
[[nodiscard]] Role role_from_string(std::string_view s);

enum class PartKind {
    text,
    file,
    data,
};

[[nodiscard]] std::string to_string(PartKind k);
[[nodiscard]] PartKind part_kind_from_string(std::string_view s);

// ============================================================================
// Core objects
// ============================================================================

struct Part {
    PartKind kind = PartKind::text;
    std::optional<std::string> text;
    std::optional<std::string> raw;        ///< base64-encoded file bytes (file kind)
    std::optional<std::string> url;        ///< file URL (file kind)
    nlohmann::json data = nullptr;         ///< arbitrary structured data (data kind)
    nlohmann::json metadata = nullptr;
    std::optional<std::string> filename;
    std::optional<std::string> media_type;

    [[nodiscard]] static Part from_text(std::string t,
                                        std::optional<std::string> media_type = std::nullopt);
    [[nodiscard]] static Part from_url(std::string u,
                                       std::optional<std::string> filename = std::nullopt,
                                       std::optional<std::string> media_type = std::nullopt);
    [[nodiscard]] static Part from_data(nlohmann::json d,
                                        std::optional<std::string> media_type = std::nullopt);
};

struct Message {
    std::string message_id;
    std::optional<std::string> context_id;
    std::optional<std::string> task_id;
    Role role = Role::unspecified;
    std::vector<Part> parts;
    nlohmann::json metadata = nullptr;
    std::vector<std::string> extensions;
    std::vector<std::string> reference_task_ids;
};

struct TaskStatus {
    TaskState state = TaskState::unspecified;
    std::optional<Message> message;
    std::optional<std::string> timestamp;  ///< ISO 8601
};

struct Artifact {
    std::string artifact_id;
    std::optional<std::string> name;
    std::optional<std::string> description;
    std::vector<Part> parts;
    nlohmann::json metadata = nullptr;
    std::vector<std::string> extensions;
};

struct Task {
    std::string id;
    std::optional<std::string> context_id;
    TaskStatus status;
    std::vector<Artifact> artifacts;
    std::vector<Message> history;
    nlohmann::json metadata = nullptr;
};

// ============================================================================
// Streaming events
// ============================================================================

struct TaskStatusUpdateEvent {
    std::string task_id;
    std::string context_id;
    TaskStatus status;
    nlohmann::json metadata = nullptr;
};

struct TaskArtifactUpdateEvent {
    std::string task_id;
    std::string context_id;
    Artifact artifact;
    bool append = false;
    bool last_chunk = false;
    nlohmann::json metadata = nullptr;
};

// ============================================================================
// Agent discovery
// ============================================================================

struct AgentInterface {
    std::string url;
    std::string transport;
    std::string protocol_version;
    std::optional<std::string> tenant;
};

struct AgentCapabilities {
    std::optional<bool> streaming;
    std::optional<bool> push_notifications;
    std::optional<bool> extended_agent_card;
    std::vector<nlohmann::json> extensions;  ///< AgentExtension objects
};

struct AgentSkill {
    std::string id;
    std::string name;
    std::string description;
    std::vector<std::string> tags;
    std::vector<std::string> examples;
    std::vector<std::string> input_modes;
    std::vector<std::string> output_modes;
};

struct AgentCard {
    std::string name;
    std::string description;
    std::vector<AgentInterface> supported_interfaces;
    nlohmann::json provider = nullptr;  ///< AgentProvider object
    std::string version;
    std::optional<std::string> documentation_url;
    AgentCapabilities capabilities;
    nlohmann::json security_schemes = nullptr;
    nlohmann::json security_requirements = nullptr;
    std::vector<std::string> default_input_modes;
    std::vector<std::string> default_output_modes;
    std::vector<AgentSkill> skills;
    nlohmann::json signatures = nullptr;
    std::optional<std::string> icon_url;
};

// ============================================================================
// Request / response helpers
// ============================================================================

struct SendMessageConfiguration {
    std::vector<std::string> accepted_output_modes;
    std::optional<int> history_length;
    bool return_immediately = false;
};

struct SendMessageRequest {
    std::optional<std::string> tenant;
    Message message;
    std::optional<SendMessageConfiguration> configuration;
    nlohmann::json metadata = nullptr;
};

struct SendMessageResult {
    std::optional<Task> task;
    std::optional<Message> message;

    [[nodiscard]] bool is_task() const noexcept { return task.has_value(); }
};

struct GetTaskRequest {
    std::optional<std::string> tenant;
    std::string id;
    std::optional<int> history_length;
};

struct ListTasksRequest {
    std::optional<std::string> tenant;
    std::optional<std::string> context_id;
    std::optional<TaskState> status;
    std::optional<int> page_size;
    std::optional<std::string> page_token;
    std::optional<int> history_length;
    std::optional<std::string> status_timestamp_after;
    std::optional<bool> include_artifacts;
};

struct ListTasksResponse {
    std::vector<Task> tasks;
    std::string next_page_token;
    int page_size = 0;
    int total_size = 0;
};

struct CancelTaskRequest {
    std::optional<std::string> tenant;
    std::string id;
    nlohmann::json metadata = nullptr;
};

struct SubscribeToTaskRequest {
    std::optional<std::string> tenant;
    std::string id;
};

// ============================================================================
// Errors
// ============================================================================

struct A2aError {
    int http_status = 0;
    std::string code;      ///< A2A error reason, e.g. "TASK_NOT_FOUND"
    std::string message;
    nlohmann::json details = nullptr;

    [[nodiscard]] std::string what() const;
};

template <typename T>
using A2aResult = std::expected<T, A2aError>;

// ============================================================================
// Streaming event handler
// ============================================================================

class IA2aEventHandler {
public:
    virtual ~IA2aEventHandler() = default;

    virtual void on_task(const Task&) {}
    virtual void on_message(const Message&) {}
    virtual void on_status_update(const TaskStatusUpdateEvent&) {}
    virtual void on_artifact_update(const TaskArtifactUpdateEvent&) {}
    virtual void on_error(const A2aError&) {}
    virtual void on_complete() {}
};

} // namespace hive::a2a
