#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>
#include <sqlite3.h>

/**
 * ConnectorStore — Phase 23: OAuth2 connector registrations.
 *
 * A connector describes an external service's OAuth app (auth/token
 * endpoints, client credentials, scopes). The authorization-code flow:
 *
 *   1. UI calls GET /api/connectors/:name/auth-url → we mint a one-shot
 *      `state` (this DB, 10-minute expiry) and return the provider URL.
 *   2. User authorizes; provider redirects to GET /api/oauth/callback
 *      with ?code&state. The callback is UNAUTHENTICATED (browser
 *      redirect carries no bearer) — the state token IS the credential.
 *   3. We exchange code → tokens and store them in the secret broker as
 *      <NAME>_ACCESS_TOKEN / <NAME>_REFRESH_TOKEN, so the entire Phase
 *      15/22 delivery machinery (env injection, browser fill, per-agent
 *      grants) applies unchanged.
 *
 * DB file: ~/.local/share/hivemind/connectors.db (fresh connection per op).
 */
struct Connector {
    std::string name;          // [a-z0-9-], 1-32 chars
    std::string auth_url;      // provider authorize endpoint
    std::string token_url;     // provider token endpoint
    std::string client_id;
    std::string client_secret; // server-side only — never listed
    std::string scopes;        // space-separated, may be ""
    std::string created_at;
};

class ConnectorStore {
public:
    ConnectorStore();                                   // default XDG path
    explicit ConnectorStore(std::filesystem::path db);  // tests
    ~ConnectorStore();

    /// Register a connector. Throws std::invalid_argument on bad input,
    /// returns false on duplicate name.
    bool create(const Connector& c);
    std::optional<Connector> get(const std::string& name);
    std::vector<Connector> list();
    bool remove(const std::string& name);

    /// Mint a pending state for the connector. "" when connector unknown.
    std::string issue_state(const std::string& connector_name);
    /// One-shot consume: returns the connector name if the state exists,
    /// is fresh (<10 min), and deletes it. nullopt otherwise.
    std::optional<std::string> consume_state(const std::string& state);

    /// [a-z0-9-], 1-32 — connector names become env-style secret names.
    static bool valid_name(const std::string& n);
    /// https:// always; http:// only for loopback (local dev providers).
    static bool valid_endpoint(const std::string& url);

private:
    std::filesystem::path db_path_;
    void ensure_schema();
    sqlite3* open_conn();
};
