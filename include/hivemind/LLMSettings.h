#pragma once

#include <string>
#include <vector>
#include <optional>
#include <unordered_map>

/**
 * LLMSettings — reads LLM model configurations from
 * ~/.config/hivemind/settings.json (preferred) or
 * ~/.codemax/settings/settings.json (shared with codemax).
 * A specific path can be forced with $HIVEMIND_SETTINGS.
 *
 * Falls back to environment variables HIVEMIND_BASE_URL,
 * HIVEMIND_API_KEY, HIVEMIND_MODEL if the file is absent.
 */

struct ModelConfig {
    std::string name;          // display name, e.g. "minimax"
    std::string model;         // model ID sent to API, e.g. "MiniMax-M3"
    std::string base_url;      // API base URL, e.g. "https://api.minimax.chat"
    std::string api_key;       // API key
    std::string chat_endpoint = "/v1/chat/completions";
    int max_context_size = 200000;
};

class LLMSettings {
public:
    LLMSettings();

    /** Reload from disk. Call after external changes. */
    void reload();

    /** True if settings loaded successfully from file or env. */
    bool loaded() const { return loaded_; }

    /** Get all configured models. */
    const std::unordered_map<std::string, ModelConfig>& models() const { return models_; }

    /** Get the default model config. */
    std::optional<ModelConfig> default_model() const;

    /** Get a model by name. */
    std::optional<ModelConfig> get_model(const std::string& name) const;

    /** Check if a model name is known. */
    bool model_exists(const std::string& name) const;

    /** Get the path of the settings file. */
    std::string settings_path() const { return settings_path_; }

    /** Errors encountered during loading. */
    const std::string& last_error() const { return last_error_; }

private:
    void load_from_file();
    void load_from_env();

    bool loaded_ = false;
    std::string settings_path_;
    std::string last_error_;
    std::unordered_map<std::string, ModelConfig> models_;
    std::string default_model_name_;
};

/** Global singleton. */
LLMSettings& llm_settings();
