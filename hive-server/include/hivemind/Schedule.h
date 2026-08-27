#pragma once

#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <string_view>

/**
 * Scheduled-trigger time computation — UTC everywhere (2026-08-27).
 *
 * Schedule specs (stored in triggers.config_json under "schedule"):
 *   {"type":"once",     "at":"2026-08-09T14:30"}       — fire once at a UTC datetime
 *   {"type":"interval", "every_minutes":30}            — fire every N minutes
 *   {"type":"daily",    "time":"09:00"}                — fire every day at HH:MM UTC
 *   {"type":"weekly",   "days":[1,3,5], "time":"09:00"} — weekdays (0=Sun) at HH:MM UTC
 *
 * All timestamps are ISO UTC "YYYY-MM-DDTHH:MM:SS" (lexicographically
 * comparable), computed with gmtime_r/timegm — never the host's local TZ.
 * The scheduler fires triggers whose next_fire_at <= now.
 */

using json = nlohmann::json;

namespace hm {

/// Current UTC time as ISO "YYYY-MM-DDTHH:MM:SS".
std::string now_iso_local();   // name kept for API stability; returns UTC

/// Next fire time strictly after `reference` (usually max(now, last_fired)),
/// or nullopt if the schedule never fires again (e.g. "once" already fired).
/// `created_at` is used as the interval anchor before the first fire.
std::optional<std::string> compute_next_fire(const json& schedule,
                                             std::string_view reference,
                                             std::string_view created_at);

/// Human-readable description, e.g. "Daily at 09:00", "Weekly on Mon, Wed at 09:00".
std::string describe_schedule(const json& schedule);

/// Validate a schedule spec. Returns "" if valid, else an error message.
std::string validate_schedule(const json& schedule);

/// Skip-if-running policy (Phase 17): what the scheduler should do with a
/// due routine whose thread already has a turn in flight.
///   Fire            — thread free: post the prompt as usual.
///   SkipReschedule  — busy recurring routine: drop THIS fire and advance
///                     next_fire_at (cron-style overlap skip; piling prompts
///                     into a busy thread would machine-gun the agent the
///                     moment its turn ends).
///   RetryLater      — busy once-routine: leave next_fire_at untouched so
///                     the next tick retries (a one-shot must not be lost).
enum class RoutineFireAction { Fire, SkipReschedule, RetryLater };
RoutineFireAction routine_fire_action(const json& schedule, bool thread_busy);

} // namespace hm
