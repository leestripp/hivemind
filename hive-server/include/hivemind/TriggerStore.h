#pragma once

// Durable, one-shot agent event subscriptions. The first supported event is
// agent_idle; the store deliberately keeps the event type as data so later
// event sources do not require another schema migration.

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>
#include <sqlite3.h>

struct EventTrigger {
    std::string id;
    std::string agent_id;         // watcher / owner
    std::string thread_id;        // solo trigger thread
    std::string event_type;       // initially only "agent_idle"
    std::string target_agent_id;  // watched agent
    std::string prompt;           // optional instructions for the wake
    std::string created_at;
    bool claimed = false;         // delivery in progress; recovered on boot
};

nlohmann::json trigger_json(const EventTrigger& trigger);

class TriggerStore {
public:
    TriggerStore();
    explicit TriggerStore(std::filesystem::path db);
    ~TriggerStore();

    EventTrigger create(const std::string& agent_id,
                        const std::string& thread_id,
                        const std::string& event_type,
                        const std::string& target_agent_id,
                        const std::string& prompt);
    std::optional<EventTrigger> get(const std::string& id);
    std::vector<EventTrigger> list(const std::string& agent_id = "");

    /// Atomically claim every unclaimed subscription for one event target.
    /// Only one caller can receive a given row. Completion removes it;
    /// failure releases it so a later idle transition can retry.
    std::vector<EventTrigger> claim(const std::string& event_type,
                                    const std::string& target_agent_id);
    bool complete(const std::string& id);
    bool release(const std::string& id);

    bool remove(const std::string& id);
    /// Remove subscriptions and background activity for this agent.
    int remove_for_agent(const std::string& agent_id);

    /// Durable worker-reported activity used by event idle predicates.
    void set_background_active(const std::string& agent_id, int active_count);
    int background_active(const std::string& agent_id) const;

private:
    std::filesystem::path db_path_;
    void ensure_schema();
    sqlite3* open_conn() const;
};
