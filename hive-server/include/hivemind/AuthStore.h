#pragma once

#include <optional>
#include <string>
#include <vector>
#include <filesystem>
#include <sqlite3.h>

/**
 * AuthStore — API key authentication (HiveMind 2.0, Plan.md Phase 9).
 *
 * Token-based auth for every /api/* route (except /api/health). Keys are
 * generated with OmniNet's TokenUtils (CSPRNG) and only the SHA-256 hash is
 * stored — the raw key is shown exactly once at creation.
 *
 * Scopes (comma-separated in the row):
 *   admin — everything, incl. key management + container/exec debug
 *   agent — all roster/chat/routine/skill/graph APIs (no keys, no exec)
 *   read  — GET only
 *
 * On first run (empty table) a master key is bootstrapped: printed to
 * stdout once and stored hashed. Without it nothing but /api/health answers.
 *
 * DB file: ~/.local/share/hivemind/auth.db (fresh connection per op).
 */
struct ApiKeyEntry {
    std::string id;          // public key id (prefix, safe to display)
    std::string name;        // human label ("master", "laptop", ...)
    std::string scopes;      // comma-separated
    std::string created_at;
    bool revoked = false;
};

class AuthStore {
public:
    AuthStore();                                  // default XDG path
    explicit AuthStore(std::filesystem::path db); // tests / custom path
    ~AuthStore();

    /// Bootstrap the master key if the store is empty. If `print` is true
    /// the raw key is written to stdout (server startup); it is never
    /// recoverable afterwards. Returns true when a new master was created.
    bool ensure_master_key(bool print);

    /// Validate a raw key. Returns its scope list, or nullopt when unknown
    /// or revoked.
    std::optional<std::vector<std::string>> validate(const std::string& raw_key);

    /// Create a key; returns the RAW key (show once). id/name/scopes stored.
    std::string create_key(const std::string& name, const std::string& scopes);

    bool revoke(const std::string& id);
    /// Phase 59: revoked keys are a state, not a tombstone — they can be
    /// reactivated (undo) or permanently deleted.
    bool reactivate(const std::string& id);
    /// True row removal. Revoked-only by design (two-step safety) and the
    /// master key can never be deleted (unrecoverable).
    bool delete_key(const std::string& id);
    std::vector<ApiKeyEntry> list();

    /// Scope helpers.
    static bool has_scope(const std::vector<std::string>& scopes,
                          const std::string& need);
    static std::vector<std::string> split_scopes(const std::string& csv);

private:
    std::filesystem::path db_path_;
    void ensure_schema();
    sqlite3* open_conn();
};
