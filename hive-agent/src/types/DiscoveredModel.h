/**
 * @file hive-brain/types/DiscoveredModel.h
 * @brief Read-only description of a model returned by a provider's
 *        model-discovery endpoint.
 */
#pragma once

#include <cstdint>
#include <string>

namespace hive {

struct DiscoveredModel {
    std::string id;        ///< Model identifier sent in chat requests.
    std::string owned_by;  ///< Provider or organization that owns the model.
    int64_t created = 0;   ///< Unix timestamp (seconds) when the model was created.

    // Optional capability fields returned by some OpenAI-compatible providers
    // (e.g., Moonshot/Kimi). Default values mean "not reported by provider".
    int64_t context_length = 0;   ///< Maximum context length in tokens.
    bool supports_vision = false; ///< Supports image/video input.
    bool supports_reasoning = false; ///< Supports reasoning/thinking mode.
};

}  // namespace hive
