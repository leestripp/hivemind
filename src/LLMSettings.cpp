#include "hivemind/LLMSettings.h"
#include <nlohmann/json.hpp>
#include <fstream>
#include <vector>
#include <sstream>
#include <filesystem>

namespace fs = std::filesystem;

LLMSettings::LLMSettings() {
    // Try file first, then env vars
    load_from_file();
    if (!loaded_) {
        load_from_env();
    }
}

void LLMSettings::reload() {
    models_.clear();
    default_model_name_.clear();
    last_error_.clear();
    loaded_ = false;
    load_from_file();
    if (!loaded_) {
        load_from_env();
    }
}

void LLMSettings::load_from_file() {
    fs::path path;

    // Search a few sensible locations. The first one that exists wins.
    const char* home = std::getenv("HOME");
    if (home) {
        std::vector<fs::path> candidates = {
            fs::path{home} / ".config" / "hivemind" / "settings.json",
            fs::path{home} / ".codemax" / "settings" / "settings.json",
        };
        // Also allow a fully-qualified override.
        if (const char* override = std::getenv("HIVEMIND_SETTINGS")) {
            candidates.insert(candidates.begin(), fs::path{override});
        }
        for (const auto& c : candidates) {
            if (fs::exists(c)) { path = c; break; }
        }
    }

    if (path.empty()) {
        last_error_ = "settings file not found (looked in ~/.config/hivemind/settings.json, ~/.codemax/settings/settings.json, and $HIVEMIND_SETTINGS)";
        return;
    }

    settings_path_ = path.string();

    std::ifstream f(path);
    if (!f) {
        last_error_ = "cannot open " + settings_path_;
        return;
    }

    std::stringstream ss;
    ss << f.rdbuf();

    try {
        auto j = nlohmann::json::parse(ss.str());

        // Load default model name
        default_model_name_ = j.value("default_model", "");

        // Load models dict
        if (j.contains("models") && j["models"].is_object()) {
            for (auto& [name, cfg] : j["models"].items()) {
                ModelConfig mc;
                mc.name = name;
                mc.model       = cfg.value("model",       "");
                mc.base_url    = cfg.value("host",        "");
                mc.api_key     = cfg.value("api_key",     "");
                mc.chat_endpoint = cfg.value("chat_endpoint", "/v1/chat/completions");
                mc.max_context_size = cfg.value("max_context_size", 200000);
                if (!mc.api_key.empty()) {
                    // Expand $ENV_NAME env-var references
                    if (mc.api_key.starts_with("$")) {
                        const char* env_val = std::getenv(mc.api_key.c_str() + 1);
                        if (env_val) mc.api_key = env_val;
                    }
                }
                models_[name] = std::move(mc);
            }
        }

        // If no default set, use the first model
        if (default_model_name_.empty() && !models_.empty()) {
            default_model_name_ = models_.begin()->first;
        }

        loaded_ = true;

    } catch (const nlohmann::json::parse_error& e) {
        last_error_ = std::string{"JSON parse error: "} + e.what();
    } catch (const std::exception& e) {
        last_error_ = e.what();
    }
}

void LLMSettings::load_from_env() {
    const char* url = std::getenv("HIVEMIND_BASE_URL");
    const char* key = std::getenv("HIVEMIND_API_KEY");
    const char* model = std::getenv("HIVEMIND_MODEL");

    if (!url || !key || !model) {
        last_error_ = "env vars HIVEMIND_BASE_URL/API_KEY/MODEL not all set";
        return;
    }

    ModelConfig mc;
    mc.name       = "env";
    mc.model      = model;
    mc.base_url   = url;
    mc.api_key    = key;
    mc.chat_endpoint = "/v1/chat/completions";
    models_["env"] = std::move(mc);
    default_model_name_ = "env";
    loaded_ = true;
    last_error_.clear();
}

std::optional<ModelConfig> LLMSettings::default_model() const {
    if (default_model_name_.empty()) return std::nullopt;
    auto it = models_.find(default_model_name_);
    if (it == models_.end()) return std::nullopt;
    return it->second;
}

std::optional<ModelConfig> LLMSettings::get_model(const std::string& name) const {
    auto it = models_.find(name);
    if (it == models_.end()) return std::nullopt;
    return it->second;
}

bool LLMSettings::model_exists(const std::string& name) const {
    return models_.contains(name);
}

// Singleton
static LLMSettings* g_settings = nullptr;

LLMSettings& llm_settings() {
    if (!g_settings) g_settings = new LLMSettings();
    return *g_settings;
}
