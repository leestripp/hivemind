// src/Schedule.cpp — scheduled-trigger time computation.
// UTC everywhere (2026-08-27): schedules are entered and fired in UTC
// regardless of the host's TZ (gmtime_r/timegm, never localtime/mktime).
#include "hivemind/Schedule.h"

#include <cstdio>
#include <ctime>
#include <vector>

namespace hm {

namespace {

std::string tm_to_iso(const std::tm& t) {
    char buf[24];
    std::snprintf(buf, sizeof buf, "%04d-%02d-%02dT%02d:%02d:%02d",
                  t.tm_year + 1900, t.tm_mon + 1, t.tm_mday,
                  t.tm_hour, t.tm_min, t.tm_sec);
    return buf;
}

std::string time_t_to_iso(std::time_t tt) {
    std::tm t{};
    gmtime_r(&tt, &t);
    return tm_to_iso(t);
}

// Parse "YYYY-MM-DDTHH:MM[:SS]" as UTC. Returns time_t or -1.
std::time_t iso_to_time_t(std::string_view iso) {
    if (iso.size() < 16) return -1;
    std::tm t{};
    t.tm_year = std::stoi(std::string(iso.substr(0, 4))) - 1900;
    t.tm_mon  = std::stoi(std::string(iso.substr(5, 2))) - 1;
    t.tm_mday = std::stoi(std::string(iso.substr(8, 2)));
    t.tm_hour = std::stoi(std::string(iso.substr(11, 2)));
    t.tm_min  = std::stoi(std::string(iso.substr(14, 2)));
    t.tm_sec  = iso.size() >= 19 ? std::stoi(std::string(iso.substr(17, 2))) : 0;
    t.tm_isdst = -1;
    return ::timegm(&t);
}

bool valid_hhmm(std::string_view s) {
    if (s.size() != 5 || s[2] != ':') return false;
    for (size_t i : {0UL, 1UL, 3UL, 4UL}) if (s[i] < '0' || s[i] > '9') return false;
    int h = std::stoi(std::string(s.substr(0, 2)));
    int m = std::stoi(std::string(s.substr(3, 2)));
    return h >= 0 && h <= 23 && m >= 0 && m <= 59;
}

// Next occurrence of HH:MM strictly after ref, on any day (daily)
// or on a listed weekday (weekly, days empty → daily).
std::time_t next_daily(std::time_t ref, std::string_view hhmm,
                       const std::vector<int>& days) {
    int hh = std::stoi(std::string(hhmm.substr(0, 2)));
    int mm = std::stoi(std::string(hhmm.substr(3, 2)));
    // Scan forward up to 8 days (covers weekly sets; UTC → no DST edges).
    std::tm base{};
    gmtime_r(&ref, &base);
    base.tm_hour = hh; base.tm_min = mm; base.tm_sec = 0; base.tm_isdst = -1;
    for (int d = 0; d < 8; ++d) {
        std::tm cand = base;
        cand.tm_mday += d;
        cand.tm_isdst = -1;
        std::time_t ct = ::timegm(&cand);
        std::tm norm{};
        gmtime_r(&ct, &norm);
        if (ct <= ref) continue;
        if (days.empty()) return ct;                       // daily
        for (int want : days) if (norm.tm_wday == want) return ct;
    }
    return -1;
}

} // namespace

std::string now_iso_local() {
    return time_t_to_iso(std::time(nullptr));
}

std::string validate_schedule(const json& sch) {
    if (!sch.is_object() || !sch.contains("type") || !sch["type"].is_string())
        return "schedule.type is required";
    const std::string type = sch["type"];
    if (type == "once") {
        if (!sch.contains("at") || !sch["at"].is_string()
            || iso_to_time_t(sch["at"].get<std::string>()) == -1)
            return "once schedule needs at:\"YYYY-MM-DDTHH:MM\"";
        return "";
    }
    if (type == "interval") {
        if (!sch.contains("every_minutes") || !sch["every_minutes"].is_number_integer()
            || sch["every_minutes"].get<long>() < 1)
            return "interval schedule needs every_minutes >= 1";
        return "";
    }
    if (type == "daily" || type == "weekly") {
        if (!sch.contains("time") || !sch["time"].is_string()
            || !valid_hhmm(sch["time"].get<std::string>()))
            return type + " schedule needs time:\"HH:MM\"";
        if (type == "weekly") {
            if (!sch.contains("days") || !sch["days"].is_array() || sch["days"].empty())
                return "weekly schedule needs days:[0..6] (0=Sunday)";
            for (auto& d : sch["days"]) {
                if (!d.is_number_integer() || d.get<int>() < 0 || d.get<int>() > 6)
                    return "weekly days must be integers 0..6";
            }
        }
        return "";
    }
    return "unknown schedule type: " + type;
}

std::optional<std::string> compute_next_fire(const json& sch,
                                             std::string_view reference,
                                             std::string_view created_at) {
    if (validate_schedule(sch) != "") return std::nullopt;
    const std::string type = sch["type"];
    const std::time_t ref = iso_to_time_t(reference);
    if (ref == -1) return std::nullopt;

    if (type == "once") {
        // Fires at the given time even if in the past (scheduler catches up once).
        std::time_t at = iso_to_time_t(sch["at"].get<std::string>());
        if (at == -1) return std::nullopt;
        return time_t_to_iso(at);
    }
    if (type == "interval") {
        const long mins = sch["every_minutes"].get<long>();
        std::time_t anchor = iso_to_time_t(created_at);
        if (anchor == -1) anchor = ref;
        std::time_t next = anchor + mins * 60;
        // Skip missed occurrences (server was down etc.) — no catch-up storm.
        while (next <= ref) next += mins * 60;
        return time_t_to_iso(next);
    }
    // daily / weekly
    std::vector<int> days;
    if (type == "weekly")
        for (auto& d : sch["days"]) days.push_back(d.get<int>());
    std::time_t next = next_daily(ref, sch["time"].get<std::string>(), days);
    if (next == -1) return std::nullopt;
    return time_t_to_iso(next);
}

RoutineFireAction routine_fire_action(const json& sch, bool thread_busy,
                                      bool worker_online) {
    if (!thread_busy && worker_online) return RoutineFireAction::Fire;
    const bool once = sch.is_object() && sch.value("type", "") == "once";
    return once ? RoutineFireAction::RetryLater
                : RoutineFireAction::SkipReschedule;
}

std::string describe_schedule(const json& sch) {
    if (!sch.is_object() || !sch.contains("type")) return "invalid schedule";
    const std::string type = sch.value("type", "");
    if (type == "once")
        return "Once at " + sch.value("at", "?");
    if (type == "interval") {
        long m = sch.value("every_minutes", 0L);
        if (m == 1) return "Every minute";
        if (m % (60 * 24) == 0) {  // check days before hours (1440 % 60 == 0)
            long d = m / (60 * 24);
            return "Every " + std::to_string(d) + " day" + (d > 1 ? "s" : "");
        }
        if (m % 60 == 0) return "Every " + std::to_string(m / 60) + " hour" + (m / 60 > 1 ? "s" : "");
        return "Every " + std::to_string(m) + " minutes";
    }
    if (type == "daily")
        return "Daily at " + sch.value("time", "?");
    if (type == "weekly") {
        static const char* names[7] = {"Sun","Mon","Tue","Wed","Thu","Fri","Sat"};
        std::string ds;
        if (sch.contains("days") && sch["days"].is_array()) {
            for (auto& d : sch["days"]) {
                int w = d.get<int>();
                if (w >= 0 && w <= 6) {
                    if (!ds.empty()) ds += ", ";
                    ds += names[w];
                }
            }
        }
        return "Weekly on " + ds + " at " + sch.value("time", "?");
    }
    return "unknown schedule";
}

} // namespace hm
