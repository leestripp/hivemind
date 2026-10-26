/**
 * @file AgentState.cpp
 * @brief Implementation of hive::to_string(AgentState).
 */
#include "types/AgentState.h"

#include <cctype>
#include <cstddef>
#include <string_view>

namespace hive {

const char* to_string(AgentState state) noexcept {
    switch (state) {
        case AgentState::Idle:             return "Idle";
        case AgentState::Understanding:    return "Understanding";
        case AgentState::Planning:         return "Planning";
        case AgentState::Acting:           return "Acting";
        case AgentState::Verifying:        return "Verifying";
        case AgentState::Documenting:      return "Documenting";
        case AgentState::Compressing:      return "Compressing";
        case AgentState::Done:             return "Done";
        case AgentState::Cancelled:        return "Cancelled";
        case AgentState::Error:            return "Error";
    }
    return "Unknown";
}

AgentState agent_state_from(std::string_view name, bool* ok) noexcept {
    auto eq_ci = [&](const char* s) {
        if (name.size() != std::char_traits<char>::length(s)) return false;
        for (size_t i = 0; i < name.size(); ++i) {
            char a = static_cast<char>(std::tolower(static_cast<unsigned char>(name[i])));
            char b = static_cast<char>(std::tolower(static_cast<unsigned char>(s[i])));
            if (a != b) return false;
        }
        return true;
    };
    if (eq_ci("Idle"))             { if (ok) *ok = true;  return AgentState::Idle; }
    if (eq_ci("Understanding"))    { if (ok) *ok = true;  return AgentState::Understanding; }
    if (eq_ci("Planning"))         { if (ok) *ok = true;  return AgentState::Planning; }
    if (eq_ci("Acting"))           { if (ok) *ok = true;  return AgentState::Acting; }
    if (eq_ci("Verifying"))         { if (ok) *ok = true;  return AgentState::Verifying; }
    if (eq_ci("Documenting"))       { if (ok) *ok = true;  return AgentState::Documenting; }
    if (eq_ci("Compressing"))      { if (ok) *ok = true;  return AgentState::Compressing; }
    if (eq_ci("Done"))             { if (ok) *ok = true;  return AgentState::Done; }
    if (eq_ci("Cancelled"))        { if (ok) *ok = true;  return AgentState::Cancelled; }
    if (eq_ci("Error"))            { if (ok) *ok = true;  return AgentState::Error; }
    if (ok) *ok = false;
    return AgentState::Idle;
}

}  // namespace hive