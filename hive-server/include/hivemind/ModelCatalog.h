#pragma once

#include <nlohmann/json.hpp>

#include <string_view>

namespace hivemind {

/// Registry defaults resolved during provider discovery. Zero means the
/// provider did not report a value and the static catalog has no answer.
struct ModelMetadata {
    int max_context = 0;
    double temperature = 0.0;
};

/// Resolve one /v1/models item. Provider-reported values win; the hub's
/// static Moonshot/MiniMax catalog fills omissions. The hub deliberately
/// does not link hive-agent, so this small metadata mirror lives here.
ModelMetadata model_metadata(std::string_view plugin, std::string_view host,
                             std::string_view model_id,
                             const nlohmann::json* discovery_item = nullptr);

} // namespace hivemind
