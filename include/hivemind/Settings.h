#pragma once

#include <string>
#include <vector>
#include <optional>
#include <unordered_map>

/**
 * Settings — server configuration for HiveMind.
 *
 * Lightweight: port number only. Model configs come from
 * ~/.codemax/settings/settings.json (read-only).
 */
struct Settings {
    std::string address = "0.0.0.0";
    int port = 8421;
};

Settings load_settings(int argc, char* argv[]);

/**
 * Read available model names from ~/.codemax/settings/settings.json.
 * Returns empty vector if file missing or unreadable.
 */
std::vector<std::string> available_models();

/**
 * Check if a model name exists in ~/.codemax/settings/settings.json.
 */
bool model_exists(const std::string& name);
