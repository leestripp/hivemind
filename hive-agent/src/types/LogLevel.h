/**
 * @file hive-brain/LogLevel.h
 * @brief Severity levels for log messages emitted by the Agent to AgentSink.
 *
 * Part of the public C++ API of the the hive-agent engine static library.
 * Stable across minor versions.
 */
#pragma once

#include <cstdint>

namespace hive {

enum class LogLevel : std::uint8_t {
    Trace,
    Debug,
    Info,
    Warn,
    Error,
    Fatal,
};

/// @brief Human-readable name (e.g. "Info"). Never returns nullptr.
const char* to_string(LogLevel level) noexcept;

}  // namespace hive