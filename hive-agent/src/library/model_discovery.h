/**
 * @file hive-brain/model_discovery.h
 * @brief Provider-agnostic model discovery for OpenAI-compatible endpoints.
 *
 * Queries a provider's `GET /v1/models` endpoint and returns the list of
 * models exposed there. Works for OpenAI, Moonshot (Kimi), MiniMax, and any
 * other provider that implements the OpenAI model-list contract.
 */
#pragma once

#include <string>
#include <vector>

#include "types/DiscoveredModel.h"

namespace hive {

struct ResolvedModelConfig;  // Defined in hive-brain/settings.h

/// @brief Result of a model-discovery request.
struct ModelDiscoveryResult {
    bool success = false;          ///< True when models were fetched and parsed.
    std::string error_message;     ///< Human-readable error when success == false.
    std::vector<DiscoveredModel> models;  ///< Discovered models when success == true.
};

/**
 * @brief Discover models at the endpoint described by `endpoint`.
 *
 * The discovery URL is `endpoint.host + endpoint.discovery_endpoint`.
 *
 * @param endpoint  A ResolvedModelConfig with at least `host` and `api_key` populated.
 * @return          ModelDiscoveryResult containing the models or an error.
 */
ModelDiscoveryResult discover_models(const ResolvedModelConfig& endpoint);

/**
 * @brief Discover models at an arbitrary OpenAI-compatible endpoint.
 *
 * @param host     Base URL of the provider, e.g. "https://api.example.com".
 * @param api_key  API key used as `Authorization: Bearer <api_key>`.
 * @param path     Discovery path, default "/v1/models".
 * @return         ModelDiscoveryResult containing the models or an error.
 */
ModelDiscoveryResult discover_models(std::string_view host,
                                     std::string_view api_key,
                                     std::string_view path = "/v1/models");

}  // namespace hive
