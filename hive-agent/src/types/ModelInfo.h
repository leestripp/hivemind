/**
 * @file hive-brain/ModelInfo.h
 * @brief Read-only description of a model configured in settings.json.
 */
#pragma once

#include <string>

namespace hive {

struct ModelInfo {
    std::string name;             ///< Key in settings (e.g. "minimax-m3")
    std::string provider;         ///< Provider name
    std::string model_id;         ///< Model id sent to the API
    std::string host;             ///< API host (from provider)
    std::string chat_endpoint;    ///< e.g. "/v1/chat/completions" (from provider)
    int max_context_size = 0;     ///< Effective max (resolved with fallback)
    double temperature = 0.0;     ///< Sampling temperature (model-specific)
};

}  // namespace hive