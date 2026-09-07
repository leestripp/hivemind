/**
 * @file src/library/model_discovery.cpp
 * @brief Implementation of hive::discover_models().
 */

#include "library/model_discovery.h"
#include "core/settings.h"
#include "api/ProviderPlugin.h"    // §ProviderPlugins
#include "api/ProviderRegistry.h"  // §ProviderPlugins

#include "api/HttpSession.h"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <string>
#include <vector>

using json = nlohmann::json;

namespace hive {

ModelDiscoveryResult discover_models(const ResolvedModelConfig& endpoint) {
    return discover_models(endpoint.host, endpoint.api_key,
                           endpoint.discovery_endpoint);
}

ModelDiscoveryResult discover_models(std::string_view host,
                                     std::string_view api_key,
                                     std::string_view path) {
    ModelDiscoveryResult result;

    if (host.empty()) {
        result.error_message = "Model discovery failed: host is empty";
        return result;
    }

    std::string host_str(host);
    while (!host_str.empty() && host_str.back() == '/') {
        host_str.pop_back();
    }

    // Avoid double /v1 when the host already includes the API version prefix.
    // Common configuration styles:
    //   host=https://api.example.com      + path=/v1/models  -> /v1/models
    //   host=https://api.example.com/v1   + path=/v1/models  -> /v1/models
    std::string path_str(path);
    if ((host_str.size() >= 3 && host_str.compare(host_str.size() - 3, 3, "/v1") == 0) &&
        (path_str.size() >= 3 && path_str.compare(0, 3, "/v1") == 0)) {
        path_str.erase(0, 3);  // remove the leading /v1 from path
    }

    const std::string url = host_str + path_str;

    hive::net::HttpRequest req;
    req.url = url;
    req.method = "GET";
    req.bearer_token = std::string(api_key);
    req.timeout_seconds = 30;

    auto future = hive::net::HttpSession::get_instance().submit(req);
    auto response = future.get();

    if (!response.has_value()) {
        result.error_message = "Model discovery failed: ";
        result.error_message += hive::net::to_string(response.error());
        return result;
    }

    if (!response->ok()) {
        result.error_message = "Model discovery failed: HTTP ";
        result.error_message += std::to_string(response->status_code);
        if (!response->body.empty()) {
            result.error_message += " - ";
            result.error_message += response->body.substr(0, 200);
        }
        return result;
    }

    try {
        const json doc = json::parse(response->body);

        // §ProviderPlugins: parsing is delegated to the provider's
        // plugin (auto-detected from the host; the OpenAI-compatible
        // default handles the classic {"object":"list","data":[...]}
        // shape). Providers with a different models-endpoint dialect
        // override parse_models_response().
        auto plugin = ProviderRegistry::resolve("", host_str);
        auto parsed = plugin->parse_models_response(doc);

        if (!parsed.error.empty()) {
            result.error_message = "Model discovery failed: " + parsed.error;
            return result;
        }

        for (auto& model : parsed.models) {
            if (!model.id.empty()) {
                result.models.push_back(std::move(model));
            }
        }

        result.success = true;
    } catch (const json::exception& e) {
        result.error_message = "Model discovery failed: JSON parse error - ";
        result.error_message += e.what();
    }

    return result;
}

}  // namespace hive
