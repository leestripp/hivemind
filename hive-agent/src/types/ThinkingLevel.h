/**
 * @file hive-brain/ThinkingLevel.h
 * @brief User-selectable thinking/reasoning level for chat models.
 *
 * Part of the public C++ API of the the hive-agent engine static library.
 * Stable across minor versions.
 *
 * Not every model supports every level — support is provider- and
 * model-specific (see ProviderPlugin::supported_thinking_levels()):
 *
 *   - kimi-k3 / k3 / k3-256k:  Low / High / Max (thinking always on;
 *     None is ignored — K3 cannot disable thinking).
 *   - kimi-k2.5 / kimi-k2.6:   None only (thinking on/off switch).
 *   - kimi-k2.7-code / kimi-for-coding: no control (always thinks).
 *   - MiniMax-M3:              None only (thinking on/off switch).
 *   - MiniMax M2.x:            no control (always thinks).
 *
 * Selecting an unsupported level is silently ignored — the library never
 * sends a parameter the model would reject.
 */
#pragma once

#include <cstdint>
#include <string_view>

namespace hive {

enum class ThinkingLevel : std::uint8_t {
    Default,  ///< Send nothing; provider/model default behavior.
    None,     ///< Disable thinking where the model allows it.
    Low,      ///< Reduced reasoning effort (Kimi K3 family).
    High,     ///< Elevated reasoning effort (Kimi K3 family).
    Max,      ///< Maximum reasoning effort (Kimi K3 family default).
};

/// @brief Human-readable name (e.g. "Low"). Never returns nullptr.
const char* to_string(ThinkingLevel level) noexcept;

/// @brief Parse a level name (case-insensitive: "none", "low", "high",
///        "max", "default"). Returns Default for unrecognized input.
ThinkingLevel thinking_level_from_string(std::string_view s) noexcept;

}  // namespace hive
