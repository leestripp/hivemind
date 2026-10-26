#include "hivemind/ModelCatalog.h"

#include <algorithm>
#include <cctype>
#include <limits>
#include <optional>
#include <string>

namespace hivemind {
namespace {

using json = nlohmann::json;

enum class ProviderKind { generic, moonshot, minimax };

struct CatalogEntry {
    std::string_view id;
    int context;
    double temperature;
};

// Keep in sync with hive-agent's provider plugins. These are defaults for
// the hub registry, not per-user overrides. Moonshot values follow the
// official Model Parameter Reference (2026-10): K3/K3.1 = 1M, temp 1.0;
// current K2.x = 256K. MiniMax values mirror its proven runtime table.
constexpr CatalogEntry kMoonshot[] = {
    {"kimi-k3.1-pro", 1000000, 1.0},
    {"kimi-k3.1-pro-highspeed", 1000000, 1.0},
    {"kimi-k3", 1000000, 1.0},
    {"k3", 1000000, 1.0},
    {"k3-256k", 262144, 1.0},
    {"kimi-k2.7-code", 262144, 1.0},
    {"kimi-k2.7-code-highspeed", 262144, 1.0},
    {"kimi-for-coding", 262144, 1.0},
    {"kimi-for-coding-highspeed", 262144, 1.0},
    {"kimi-k2.6", 262144, 1.0},
    {"kimi-k2.5", 262144, 0.6},
    {"moonshot-v1-8k", 8192, 0.3},
    {"moonshot-v1-32k", 32768, 0.3},
    {"moonshot-v1-128k", 131072, 0.3},
    {"moonshot-v1-8k-vision-preview", 8192, 0.3},
    {"moonshot-v1-32k-vision-preview", 32768, 0.3},
    {"moonshot-v1-128k-vision-preview", 131072, 0.3},
};

constexpr CatalogEntry kMiniMax[] = {
    {"MiniMax-M3", 500000, 1.0},
    {"MiniMax-M2.7", 200000, 1.0},
    {"MiniMax-M2.7-highspeed", 200000, 1.0},
    {"MiniMax-M2.5", 200000, 1.0},
    {"MiniMax-M2.5-highspeed", 200000, 1.0},
    {"MiniMax-M2.1", 200000, 1.0},
    {"MiniMax-M2.1-highspeed", 200000, 1.0},
    {"MiniMax-M2", 200000, 1.0},
};

std::string lower(std::string_view value) {
    std::string out(value);
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return out;
}

bool iequals(std::string_view a, std::string_view b) {
    return lower(a) == lower(b);
}

ProviderKind provider_kind(std::string_view plugin, std::string_view host) {
    const std::string p = lower(plugin);
    const std::string h = lower(host);
    if (p == "moonshot" || (p.empty() &&
        (h.find("moonshot") != std::string::npos ||
         h.find("kimi") != std::string::npos)))
        return ProviderKind::moonshot;
    if (p == "minimax" || (p.empty() &&
        h.find("minimax") != std::string::npos))
        return ProviderKind::minimax;
    return ProviderKind::generic;
}

template <size_t N>
const CatalogEntry* find_entry(const CatalogEntry (&entries)[N],
                               std::string_view model_id) {
    for (const auto& entry : entries) {
        if (iequals(entry.id, model_id)) return &entry;
    }
    return nullptr;
}

const CatalogEntry* known_entry(ProviderKind kind, std::string_view model_id) {
    if (kind == ProviderKind::moonshot)
        return find_entry(kMoonshot, model_id);
    if (kind == ProviderKind::minimax)
        return find_entry(kMiniMax, model_id);
    return nullptr;
}

int reported_context(const json& item) {
    for (const char* key : {"context_length", "max_context", "context_window",
                            "max_context_tokens", "context_size"}) {
        const auto it = item.find(key);
        if (it == item.end() || !it->is_number()) continue;
        const auto value = it->get<long long>();
        if (value > 0 && value <= std::numeric_limits<int>::max())
            return static_cast<int>(value);
    }
    return 0;
}

std::optional<double> reported_temperature(const json& item) {
    for (const char* key : {"default_temperature", "temperature"}) {
        const auto it = item.find(key);
        if (it != item.end() && it->is_number()) return it->get<double>();
    }
    return std::nullopt;
}

} // namespace

ModelMetadata model_metadata(std::string_view plugin, std::string_view host,
                             std::string_view model_id,
                             const json* discovery_item) {
    ModelMetadata out;
    const auto* known = known_entry(provider_kind(plugin, host), model_id);
    if (known) {
        out.max_context = known->context;
        out.temperature = known->temperature;
    }
    if (discovery_item && discovery_item->is_object()) {
        if (const int context = reported_context(*discovery_item); context > 0)
            out.max_context = context;
        if (const auto temperature = reported_temperature(*discovery_item))
            out.temperature = *temperature;
    }
    return out;
}

} // namespace hivemind
