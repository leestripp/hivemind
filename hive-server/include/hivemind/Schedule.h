#pragma once

#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <string_view>

/**
 * Scheduled-trigger time computation (server-local time).
 *
 * Schedule specs (stored in triggers.config_json under "schedule"):
 *   {"type":"once",     "at":"2026-08-09T14:30"}       — fire once at a local datetime
 *   {"type":"interval", "every_minutes":30}            — fire every N minutes
 *   {"type":"daily",    "time":"09:00"}                — fire every day at HH:MM
 *   {"type":"weekly",   "days":[1,3,5], "time":"09:00"} — weekdays (0=Sun) at HH:MM
 *
 * All timestamps are ISO local "YYYY-MM-DDTHH:MM:SS" (lexicographically
 * comparable). The scheduler fires triggers whose next_fire_at <= now.
 */

using json = nlohmann::json;

namespace hm {

/// Current local time as ISO "YYYY-MM-DDTHH:MM:SS".
std::string now_iso_local();

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

} // namespace hm
