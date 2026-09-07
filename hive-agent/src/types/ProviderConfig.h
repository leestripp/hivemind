/**
 * @file hive-brain/types/ProviderConfig.h
 * @brief Public provider and resolved-model configuration types.
 *
 * These structs are used by both SettingsStore and the Agent public API.
 * They intentionally contain no internal dependencies so they can be included
 * from public headers.
 */
#pragma once

#include <string>

namespace hive {

/// Shared endpoint/credential configuration for one provider.
struct ProviderConfig
{
    std::string name;               // provider label, e.g. "kimi", "minimax", "openai"
    std::string host;               // base URL, e.g. https://api.example.com
    std::string api_key;            // provider-level API key
    std::string chat_endpoint;      // default "/v1/chat/completions"
    std::string discovery_endpoint; // default "/v1/models"
    int         timeout_seconds = 30;
    std::string plugin;             // §ProviderPlugins: registered plugin name;
                                    // empty = auto-detect from name/host
};

/// One configured model. References a ProviderConfig for host/api_key/endpoints
/// and keeps only model-specific settings.
struct ModelConfig
{
    std::string name;              // key in settings ("minimax-m3", ...)
    std::string provider;          // ProviderConfig::name
    std::string model_id;          // actual model id ("MiniMax-M3", ...)
    int    max_context_size = 0;   // 0 = fall back to model's default
    double temperature     = 0.0;
};

/// Effective model settings after merging a ModelConfig with its ProviderConfig.
struct ResolvedModelConfig
{
    std::string name;
    std::string provider;
    std::string model_id;
    std::string host;
    std::string api_key;
    std::string chat_endpoint;
    std::string discovery_endpoint;
    int    timeout_seconds  = 30;
    int    max_context_size = 0;
    double temperature     = 0.0;
    std::string plugin;             // §ProviderPlugins: provider row's plugin hint
};

}  // namespace hive
