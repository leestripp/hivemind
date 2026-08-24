#pragma once

/**
 * SecretStore — Phase 15 secret broker.
 *
 * Agents need credentials at runtime (API keys, service logins) WITHOUT
 * those values ever entering the LLM context (context = history = logs =
 * leak). This store keeps secrets AES-256-GCM-encrypted at rest in
 * ~/.local/share/hivemind/secrets.db with a random master key in
 * secrets.key (0600, created on first use — same trust model as auth.db).
 *
 * Delivery channels (implemented elsewhere):
 *   - container_exec injects granted secrets as HIVE_SECRET_<NAME> env vars
 *     at exec time (ContainerManager) — never in argv, never printed.
 *   - browser fill_secret tool (Phase 6) types values via playwright; the
 *     agent's context only sees "filled".
 *
 * Per-agent grants follow the skills-toggle model: a (secret, agent) row
 * in the grants table means "this agent may receive this secret".
 */
#include <filesystem>
#include <sqlite3.h>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace hivemind {

struct SecretEntry {
    std::string name;        // [A-Za-z0-9_], 1-64 chars
    std::string created_at;
    std::string source;      // "" = local value; "pass:…"/"op://…" = Phase 22
                             // external vault reference (fetched at delivery)
    std::vector<std::string> agent_ids;  // granted agents (joined)
};

class SecretStore {
public:
    SecretStore();                                   // default XDG path
    explicit SecretStore(std::filesystem::path db,
                         std::filesystem::path key_file = {});  // tests
    ~SecretStore() = default;

    /// Create a secret. Returns false if the name exists or is invalid.
    bool put(const std::string& name, const std::string& value);
    /// Create an EXTERNALLY-SOURCED secret (Phase 22): no local value —
    /// get() fetches from the vault CLI at delivery time. The reference
    /// must pass hm::vault::validate_source. Returns false on bad
    /// name/source or duplicate.
    bool put_external(const std::string& name, const std::string& source);
    /// Decrypt a secret (broker delivery paths only — never expose via API).
    std::optional<std::string> get(const std::string& name);
    /// List names/metadata ONLY — values never leave this class through it.
    std::vector<SecretEntry> list();
    /// Delete secret + its grants.
    bool remove(const std::string& name);
    bool exists(const std::string& name);

    bool grant(const std::string& name, const std::string& agent_id);
    bool revoke(const std::string& name, const std::string& agent_id);
    std::vector<std::string> grants_for_agent(const std::string& agent_id);

    /// (HIVE_SECRET_<NAME>, value) pairs for every secret granted to the
    /// agent — ContainerManager injects these as exec env vars.
    std::vector<std::pair<std::string, std::string>>
        env_for_agent(const std::string& agent_id);

    static bool valid_name(const std::string& n);

private:
    sqlite3* open_conn();   // fresh connection per op (other stores' pattern)
    void ensure_schema(sqlite3* db);
    const std::string& master_key();  // lazily loaded 32-byte key (binary)

    std::filesystem::path db_path_;
    std::filesystem::path key_path_;
    std::string master_key_;          // 32 bytes, loaded once
};

} // namespace hivemind
