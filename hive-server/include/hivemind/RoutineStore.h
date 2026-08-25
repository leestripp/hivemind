#pragma once

#include <optional>
#include <string>
#include <vector>
#include <filesystem>
#include <sqlite3.h>

/**
 * RoutineStore — named, schedulable prompts attached to roster agents
 * (HiveMind 2.0, Plan.md Phase 5). The "watch this and ping me" feature.
 *
 * A routine fires on its schedule: the prompt is posted as a user message
 * into the routine's thread (created at routine-creation time) and the
 * agent's reply lands in the same thread — the user gets notified by the
 * client's activity/thread polling.
 *
 * Schedule specs reuse the graph-trigger format (see Schedule.h):
 *   once | interval | daily | weekly.
 *
 * DB file: ~/.local/share/hivemind/routines.db (fresh connection per op).
 */
struct Routine {
    std::string id;
    std::string agent_id;
    std::string thread_id;
    std::string name;
    std::string prompt;
    std::string schedule_json; // hm::Schedule spec
    std::string next_fire_at;  // ISO local; "" = not scheduled
    bool        enabled = true;
    std::string created_at;
    std::string last_fired_at; // "" if never
};

class RoutineStore {
public:
    RoutineStore();                                  // default XDG path
    explicit RoutineStore(std::filesystem::path db); // tests / custom path
    ~RoutineStore();

    Routine create(const std::string& agent_id, const std::string& thread_id,
                   const std::string& name, const std::string& prompt,
                   const std::string& schedule_json,
                   const std::string& next_fire_at);

    std::optional<Routine> get(const std::string& id);
    std::vector<Routine> list();
    std::vector<Routine> list_due(const std::string& now_iso_local);

    /// Record a fire: set last_fired_at + next_fire_at ("" + disable when
    /// the schedule is exhausted — same policy as graph triggers).
    void mark_fired(const std::string& id, const std::string& fired_at,
                    const std::string& next_fire_at, bool disable);

    /// Advance next_fire_at WITHOUT touching last_fired_at (Phase 17
    /// skip-if-running: the routine did not actually run). Optionally
    /// disable when the schedule is exhausted.
    void reschedule(const std::string& id, const std::string& next_fire_at,
                    bool disable);

    /// Edit name/prompt/schedule (Phase 29 routine editing UI).
    /// Caller recomputes next_fire_at from the new schedule.
    bool update(const std::string& id, const std::string& name,
                const std::string& prompt, const std::string& schedule_json,
                const std::string& next_fire_at);
    bool set_enabled(const std::string& id, bool enabled);
    bool remove(const std::string& id);
    /// Move every routine owned by old_agent to new_agent. Returns count.
    int reassign_agent(const std::string& old_agent_id,
                       const std::string& new_agent_id);
    /// Delete every routine owned by the agent. Returns count.
    int remove_for_agent(const std::string& agent_id);

private:
    std::filesystem::path db_path_;
    void ensure_schema();
    sqlite3* open_conn();
};
