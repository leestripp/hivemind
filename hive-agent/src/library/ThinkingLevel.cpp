/**
 * @file ThinkingLevel.cpp
 * @brief Implementation of hive::ThinkingLevel string helpers.
 */
#include "types/ThinkingLevel.h"

#include <algorithm>
#include <cctype>

namespace hive {

const char* to_string(ThinkingLevel level) noexcept {
    switch (level) {
        case ThinkingLevel::Default: return "Default";
        case ThinkingLevel::None:    return "None";
        case ThinkingLevel::Low:     return "Low";
        case ThinkingLevel::High:    return "High";
        case ThinkingLevel::Max:     return "Max";
    }
    return "Default";
}

ThinkingLevel thinking_level_from_string(std::string_view s) noexcept {
    auto iequals = [](std::string_view a, std::string_view b) {
        return a.size() == b.size() &&
               std::equal(a.begin(), a.end(), b.begin(),
                          [](char x, char y) {
                              return std::tolower(static_cast<unsigned char>(x)) ==
                                     std::tolower(static_cast<unsigned char>(y));
                          });
    };
    if (iequals(s, "none")) return ThinkingLevel::None;
    if (iequals(s, "low"))  return ThinkingLevel::Low;
    if (iequals(s, "high")) return ThinkingLevel::High;
    if (iequals(s, "max"))  return ThinkingLevel::Max;
    return ThinkingLevel::Default;
}

}  // namespace hive
