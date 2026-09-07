// SPDX-License-Identifier: BSL-1.0
// src/api/providers/ProviderRegistry.cpp
//
// ProviderRegistry implementation. Built-in plugins are registered
// lazily on first access (function-local statics — no static-init-order
// surprises); host plugins may be registered at any time.

#include "api/ProviderRegistry.h"

#include <algorithm>
#include <mutex>
#include <vector>

#include "api/providers/LmStudioPlugin.h"
#include "api/providers/MiniMaxPlugin.h"
#include "api/providers/MoonshotPlugin.h"
#include "api/providers/OpenAiCompatiblePlugin.h"
#include "core/Logger.h"
#include "api/ProviderPlugin.h"

namespace hive {
namespace {

std::mutex& registry_mutex() {
    static std::mutex m;
    return m;
}

std::vector<std::shared_ptr<ProviderPlugin>>& plugins() {
    static std::vector<std::shared_ptr<ProviderPlugin>> v;
    return v;
}

bool iequals(std::string_view a, std::string_view b) {
    return a.size() == b.size() &&
           std::equal(a.begin(), a.end(), b.begin(),
                      [](char x, char y) {
                          return std::tolower(static_cast<unsigned char>(x)) ==
                                 std::tolower(static_cast<unsigned char>(y));
                      });
}

// Registers the built-in plugins exactly once.
void ensure_builtins() {
    static bool done = [] {
        // Order matters: the OpenAI-compatible default stays first so
        // default_plugin() finds it by name, and specific plugins
        // registered after it win host matching over its catch-all.
        plugins().push_back(std::make_shared<OpenAiCompatiblePlugin>());
        plugins().push_back(std::make_shared<MiniMaxPlugin>());
        plugins().push_back(std::make_shared<MoonshotPlugin>());
        plugins().push_back(std::make_shared<LmStudioPlugin>());
        return true;
    }();
    (void)done;
}

} // namespace

void ProviderRegistry::register_plugin(std::shared_ptr<ProviderPlugin> plugin) {
    if (!plugin) return;
    std::scoped_lock lk(registry_mutex());
    ensure_builtins();
    const std::string n = plugin->name();
    auto& v = plugins();
    for (auto& p : v) {
        if (iequals(p->name(), n)) {
            LOG_INFO("[ProviderRegistry] replacing plugin '" + n + "'");
            p = std::move(plugin);
            return;
        }
    }
    LOG_INFO("[ProviderRegistry] registered plugin '" + n + "'");
    v.push_back(std::move(plugin));
}

std::shared_ptr<ProviderPlugin> ProviderRegistry::plugin_for_name(std::string_view name) {
    std::scoped_lock lk(registry_mutex());
    ensure_builtins();
    for (const auto& p : plugins()) {
        if (iequals(p->name(), name)) return p;
    }
    return nullptr;
}

std::shared_ptr<ProviderPlugin> ProviderRegistry::plugin_for_host(std::string_view host) {
    std::scoped_lock lk(registry_mutex());
    ensure_builtins();
    for (const auto& p : plugins()) {
        if (p->matches(host)) return p;
    }
    return nullptr;
}

std::shared_ptr<ProviderPlugin> ProviderRegistry::resolve(std::string_view provider_name,
                                                          std::string_view host) {
    if (auto p = plugin_for_name(provider_name)) return p;
    if (auto p = plugin_for_host(host)) return p;
    return default_plugin();
}

std::shared_ptr<ProviderPlugin> ProviderRegistry::default_plugin() {
    std::scoped_lock lk(registry_mutex());
    ensure_builtins();
    // The OpenAI-compatible plugin is always the first registered
    // built-in. If a host replaced it by name, the replacement wins.
    for (const auto& p : plugins()) {
        if (iequals(p->name(), "openai-compatible")) return p;
    }
    return plugins().front();
}

std::vector<std::shared_ptr<ProviderPlugin>> ProviderRegistry::all() {
    std::scoped_lock lk(registry_mutex());
    ensure_builtins();
    return plugins();
}

} // namespace hive
