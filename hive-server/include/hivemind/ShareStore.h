#pragma once

// =============================================================================
// ShareStore (2026-10-11, Plan.md "Agent-to-Agent Secure File Sharing"):
// durable METADATA registry for addressed agent-to-agent file shares.
//
// File locality (AGENTS.md) is preserved: the blob never lives on the hub —
// the sender's worker stages it at workspace/shared/<share_id>/<name> and
// fetches relay it through the chunked channel ops (file_fetch/file_push).
// This store records WHO shared WHAT with WHOM and where the staging copy
// lives, enforces the addressed-share authorization (only target or sender
// may fetch), and gives ops an audit surface (GET /api/shares).
//
// State machine: active -> revoked. Fetches update fetched_at/fetch_count
// but never change state (a share is re-fetchable while active).
// =============================================================================

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace hivemind {

/// One addressed file share. The blob lives on `worker` at `rel_path`
/// (workspace-relative); this row is metadata only.
struct Share {
    std::string id;
    std::string sender_agent_id;
    std::string target_agent_id;
    std::string filename;          // sanitized bare name
    int64_t     size_bytes = 0;
    std::string note;              // optional sender note (may be "")
    std::string worker;            // worker that holds the staged copy
    std::string rel_path;          // shared/<id>/<filename>
    std::string state;             // active | revoked
    std::string created_at;
    std::string fetched_at;        // last fetch ("" = never)
    int         fetch_count = 0;
};

class ShareStore {
public:
    ShareStore();                                   // default XDG path
    explicit ShareStore(std::filesystem::path db);  // tests / custom
    ~ShareStore();

    /// Insert a new share (state=active). `s.id` is REQUIRED (the worker
    /// generates it — it names the staging dir before the hub is called);
    /// created_at is stamped here. Throws on duplicate id / db error.
    void create_share(Share s);
    std::optional<Share> get_share(const std::string& id);
    /// Every share where the agent is sender OR target, newest first.
    /// Empty agent_id = all shares (admin audit route).
    std::vector<Share> list_for_agent(const std::string& agent_id = "",
                                      int limit = 100);
    /// Record a successful fetch: fetched_at=now, fetch_count++.
    void mark_fetched(const std::string& id);
    /// active -> revoked. True when the row existed and was active.
    bool revoke(const std::string& id);

private:
    std::filesystem::path db_path_;
    void init_schema();
};

/// Serialize a share for the bridge replies and the admin audit route
/// (metadata only — never the blob, which lives on the worker).
nlohmann::json share_to_json(const Share& s);

} // namespace hivemind
