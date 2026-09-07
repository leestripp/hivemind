/**
 * @file AgentSink.cpp
 * @brief Default AgentSink implementation (currently just the on_log
 *        fallback so the library has something to do if a host doesn't
 *        override it).
 *
 * Phase 1: minimal. Will be expanded in Phase 3 to wrap the existing
 * Logger/CLI stdout paths behind the new sink abstraction.
 */
#include "library/AgentSink.h"

#include <iostream>

#include "core/Logger.h"

// §Phase 5.1: helper macro that converts LOG_CERR(INFO,  ...); style
// statements into LOG_INFO/LOG_WARNING/LOG_ERROR calls via the
// existing Logger. Same formatted text routed through the Logger
// instead of writing to stderr directly.
#define LOG_CERR(level, expr) do {                          \
        std::ostringstream _hive_msg;                   \
        _hive_msg << expr;                               \
        LOG_##level(_hive_msg.str());                    \
    } while (0)
namespace hive {

void AgentSink::on_error(hive::ErrorKind kind,
                         std::string_view message,
                         bool will_retry) noexcept
{
    // Default: preserve pre-on_error behaviour — hosts that only override
    // on_log keep seeing every error as an Error-level log line.
    (void)kind;
    (void)will_retry;
    on_log(hive::LogLevel::Error, message);
}

void AgentSink::on_log(hive::LogLevel level,
                       std::string_view message) noexcept
{
    // Default: write to stderr. Hosts should override.
    LOG_CERR(INFO, '[' << to_string(level) << "] " << message);
}

}  // namespace hive