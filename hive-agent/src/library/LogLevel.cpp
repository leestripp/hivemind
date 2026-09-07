/**
 * @file LogLevel.cpp
 * @brief Implementation of hive::to_string(LogLevel).
 */
#include "types/LogLevel.h"

namespace hive {

const char* to_string(LogLevel level) noexcept {
    switch (level) {
        case LogLevel::Trace: return "Trace";
        case LogLevel::Debug: return "Debug";
        case LogLevel::Info:  return "Info";
        case LogLevel::Warn:  return "Warn";
        case LogLevel::Error: return "Error";
        case LogLevel::Fatal: return "Fatal";
    }
    return "Unknown";
}

}  // namespace hive