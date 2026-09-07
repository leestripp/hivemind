/**
 * @file hive-brain/ProviderRegistry.h
 * @brief Static registry of provider plugins.
 *
 * Built-in plugins (openai-compatible, minimax, moonshot, lmstudio)
 * self-register on first access. Hosts may register additional plugins
 * via `register_plugin()` before Agent construction; a registered
 * plugin becomes selectable through the providers table's `plugin`
 * column and participates in host auto-detection.
 *
 * Thread-safe; registration and lookup are mutex-guarded.
 */
#pragma once

#include <memory>
#include <string_view>
#include <vector>

namespace hive {

class ProviderPlugin;

class ProviderRegistry {
public:
    /// Register (or replace, by name) a plugin.
    static void register_plugin(std::shared_ptr<ProviderPlugin> plugin);

    /// Exact plugin-name lookup (case-insensitive). Null if unknown.
    static std::shared_ptr<ProviderPlugin> plugin_for_name(std::string_view name);

    /// First registered plugin whose matches(host) returns true.
    /// Null if none.
    static std::shared_ptr<ProviderPlugin> plugin_for_host(std::string_view host);

    /// Resolve the plugin for a configured provider: explicit name
    /// first, then host auto-detection, else the default
    /// OpenAI-compatible plugin. Never null.
    static std::shared_ptr<ProviderPlugin> resolve(std::string_view provider_name,
                                                   std::string_view host);

    /// The OpenAI-compatible default plugin. Never null.
    static std::shared_ptr<ProviderPlugin> default_plugin();

    /// All registered plugins (built-ins + host-registered).
    static std::vector<std::shared_ptr<ProviderPlugin>> all();
};

} // namespace hive
