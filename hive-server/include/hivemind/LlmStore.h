#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>
#include <sqlite3.h>

/**
 * LlmStore — Phase 43: the hub's OWN provider/model registry.
 *
 * ONE place for LLM configuration. A provider is just an API endpoint +
 * key that serves models; the hub owns the registry and injects the
 * resolved config into each task frame (HubRouter dispatch), so any
 * model can run on any machine. No per-agent or per-worker copies, no
 * relay — this store replaces all of that.
 *
 * Schema mirrors the brain's settings tables so dispatch injection maps
 * 1:1 (hive-agent upserts the frame payload into its own settings store
 * and its existing resolution machinery works unchanged).
 *
 * DB file: ~/.local/share/hivemind/llm.db (fresh connection per op).
 */
struct ProviderInfo {
    std::string name;           // primary key, e.g. "KimiCode"
    std::string host;           // e.g. "api.kimi.com"
    std::string api_key;        // plaintext here; MASKED in API responses
    std::string chat_endpoint;  // full chat-completions URL
    std::string plugin;         // provider-plugin name; "" = auto-detect
    std::string created_at;
};

struct LlmModelInfo {
    std::string name;           // primary key, e.g. "k3"
    std::string provider;       // FK → providers.name
    std::string model_id;       // actual API id, e.g. "kimi-k3-0905-preview"
    int max_context = 0;        // 0 = model default
    double temperature = 0.0;   // 0 = model default
    // Thinking/reasoning level (2026-10-01): "" = provider/model default,
    // else none|low|high|max — the hive::ThinkingLevel to_string values.
    // There is no generic provider discovery for this (see Plan.md), so it
    // is an editor field; unsupported levels are ignored by the plugin.
    std::string thinking_level;
    std::string created_at;
};

class LlmStore {
public:
    LlmStore();                                    // default XDG path
    explicit LlmStore(std::filesystem::path db);   // tests
    ~LlmStore();

    // Providers ------------------------------------------------------------
    void upsert_provider(const ProviderInfo& p);   // insert or update
    bool remove_provider(const std::string& name); // cascades its models
    std::optional<ProviderInfo> get_provider(const std::string& name);
    std::vector<ProviderInfo> list_providers();

    // Models -----------------------------------------------------------------
    void upsert_model(const LlmModelInfo& m);
    bool remove_model(const std::string& name);
    std::vector<LlmModelInfo> list_models();

    // Default model (agents with model "" resolve to this) ------------------
    void set_default_model(const std::string& name);   // "" clears
    std::string default_model();

    /// Dispatch resolution: model name ("" = default) → full provider
    /// config for frame injection. nullopt when unknown / no default.
    std::optional<ProviderInfo> resolve(const std::string& model_name,
                                        LlmModelInfo* resolved = nullptr);

private:
    std::filesystem::path db_path_;
    void ensure_schema();
};
