#pragma once

// ============================================================================
// Shared desktop/voice conversation-response validation.
//
// POST /api/threads normally returns one thread. Some deployed hubs answered
// with the full {"threads": [...]} list instead. Accept both wire shapes, but
// never guess: a conversation is usable only when its non-empty id and exact
// agent membership match the request. Falling back to threads[0] can send a
// user's instructions to the wrong agent.
// ============================================================================

#include <algorithm>
#include <stdexcept>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace hivemind {

inline bool exact_thread_agents(const nlohmann::json& thread,
                                const std::vector<std::string>& agent_ids) {
    if (!thread.is_object()) return false;
    const auto it = thread.find("agent_ids");
    if (it == thread.end() || !it->is_array() ||
        it->size() != agent_ids.size())
        return false;
    return std::all_of(agent_ids.begin(), agent_ids.end(),
                       [&it](const std::string& id) {
                           return std::find(it->begin(), it->end(), id) !=
                                  it->end();
                       });
}

inline nlohmann::json validated_thread_response(
        const nlohmann::json& response,
        const std::vector<std::string>& agent_ids) {
    auto valid = [&agent_ids](const nlohmann::json& thread) {
        const auto id = thread.find("id");
        return exact_thread_agents(thread, agent_ids) &&
               id != thread.end() && id->is_string() &&
               !id->get_ref<const std::string&>().empty();
    };

    if (response.is_object() && response.contains("id")) {
        if (valid(response)) return response;
        throw std::runtime_error(
            "hub returned a conversation for the wrong agents");
    }

    if (response.is_object()) {
        const auto threads = response.find("threads");
        if (threads != response.end() && threads->is_array()) {
            for (const auto& thread : *threads)
                if (valid(thread)) return thread;
        }
    }

    throw std::runtime_error(
        "hub did not return the requested conversation");
}

inline std::string validated_message_id(const nlohmann::json& response) {
    // Canonical hub: {"message_id": "...", "status": "processing"}.
    // Accept older single-message shapes, but never clear a draft for an
    // acknowledgement that contains no durable message identifier.
    const nlohmann::json* value = nullptr;
    if (response.is_object()) {
        const auto message_id = response.find("message_id");
        if (message_id != response.end()) value = &*message_id;
        if (!value) {
            const auto id = response.find("id");
            if (id != response.end()) value = &*id;
        }
        if (!value) {
            const auto message = response.find("message");
            if (message != response.end() && message->is_object()) {
                const auto id = message->find("id");
                if (id != message->end()) value = &*id;
            }
        }
    }
    if (value && value->is_string() &&
        !value->get_ref<const std::string&>().empty())
        return value->get<std::string>();
    throw std::runtime_error("hub did not acknowledge a stored message id");
}


} // namespace hivemind
