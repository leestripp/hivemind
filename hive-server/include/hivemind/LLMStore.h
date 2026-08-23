#pragma once

#include <optional>
#include <string>
#include <vector>
#include <filesystem>

/**
 * LLMStore — HiveMind-owned LLM provider & model configuration.
 *
 * Phase 13: this REPLACES daedalus' SettingsStore as the source of truth
 * for providers/models in HiveMind. The schema is copied from daedalus
 * (providers + models tables) but hive-server is the writer — credentials
 * are resolved here and injected into daedalus AgentConfig explicit fields
 * (api_host/api_key/chat_endpoint) at agent-spawn time, so daedalus never
 * needs to read this DB and is never patched.
 *
 * DB file: ~/.local/share/hivemind/llm.db (fresh connection per op).
 * First run with an empty store: one-way import from daedalus.db so
 * existing setups carry over.
 */
struct LlmProvider {
    std::string name;               // PK, e.g. "kimi"
    std::string host;               // base URL
    std::string api_key;
    std::string chat_endpoint;      // default "/v1/chat/completions"
    std::string discovery_endpoint; // default "/v1/models"
    int         timeout_seconds = 30;
    std::string plugin;             // daedalus provider-plugin hint; "" = auto
};

struct LlmModel {
    std::string name;               // PK — settings key, e.g. "kimi-k2.7"
    std::string provider;           // LlmProvider::name
    std::string model_id;           // actual remote id, e.g. "kimi-for-coding"
    int    max_context_size = 0;    // 0 = provider/model default
    double temperature      = 0.0;
    bool   is_default       = false;
};

/// Model merged with its provider — ready for AgentConfig injection.
struct ResolvedLlmModel {
    std::string name;        // hive settings key
    std::string model_id;    // remote model id
    std::string provider;
    std::string host;
    std::string api_key;
    std::string chat_endpoint;
    int    max_context_size = 0;
    double temperature      = 0.0;
};

class LLMStore {
public:
    LLMStore();                                  // default XDG path + import
    explicit LLMStore(std::filesystem::path db); // tests / custom path

    // ── providers ─────────────────────────────────────────────────────
    LlmProvider create_provider(const LlmProvider& p);  // throws on dupe name
    std::optional<LlmProvider> get_provider(const std::string& name);
    std::vector<LlmProvider> list_providers();
    bool update_provider(const LlmProvider& p);         // by name
    bool delete_provider(const std::string& name);      // false if models reference it

    // ── models ────────────────────────────────────────────────────────
    LlmModel create_model(const LlmModel& m);           // throws on dupe/unknown provider
    std::optional<LlmModel> get_model(const std::string& name);
    std::vector<LlmModel> list_models();
    bool update_model(const LlmModel& m);               // by name
    bool delete_model(const std::string& name);
    bool set_default_model(const std::string& name);    // clears others
    std::optional<LlmModel> default_model();

    /// Resolve a model name to full credentials. Empty name = default model.
    /// nullopt when nothing resolves (caller decides fallback policy).
    std::optional<ResolvedLlmModel> resolve(const std::string& name);

private:
    std::filesystem::path db_path_;
    void init_schema();
    void import_from_daedalus();   // only when providers table is empty
};
