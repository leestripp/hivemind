#pragma once

#include <optional>
#include <string>
#include <vector>
#include <filesystem>
#include <sqlite3.h>

/**
 * ApprovalStore — human-in-the-loop approval queue (HiveMind 2.0, Phase 3).
 *
 * When a roster agent has require_approval=true, gated actions (shell
 * commands via hive-brain' request_command_approval) create a pending row
 * here and the agent turn BLOCKS until a human resolves it via the REST
 * API (or the wait times out → auto-deny).
 *
 * DB file: ~/.local/share/hivemind/approvals.db (fresh connection per op).
 */
/// Phase 16: one ordered auto-review rule. First match wins; no match
/// falls through to the human queue (require_approval path).
struct ApprovalRule {
    std::string agent_id;
    int position = 0;
    std::string tool_pattern;      // glob on kind ("command", "browser", …)
    std::string command_pattern;   // glob on payload (the command/action)
    std::string action;            // "allow" | "deny"
};

struct Approval {
    std::string id;
    std::string agent_id;
    std::string agent_name;
    std::string kind;        // "command" (more kinds later: send_email, ...)
    std::string payload;     // the command / action awaiting review
    std::string status;      // "pending" | "approved" | "denied" | "expired"
    std::string created_at;
    std::string resolved_at; // "" while pending
};

class ApprovalStore {
public:
    ApprovalStore();                                  // default XDG path
    explicit ApprovalStore(std::filesystem::path db); // tests / custom path
    ~ApprovalStore();

    /// Create a pending approval; returns its id.
    std::string create(const std::string& agent_id, const std::string& agent_name,
                       const std::string& kind, const std::string& payload);

    std::optional<Approval> get(const std::string& id);
    std::vector<Approval> list(const std::string& status = "");  // "" = all

    /// Resolve a pending approval. Returns false if not found / not pending.
    bool resolve(const std::string& id, bool approved);

    /// Block until the approval is resolved. Polls the DB every 250 ms.
    /// Returns true=approved, false=denied/expired/timeout (timeout_s max wait).
    bool wait(const std::string& id, int timeout_s = 600);

    // ── Phase 16: rule-based auto-review ─────────────────────────────
    std::vector<ApprovalRule> list_rules(const std::string& agent_id);
    /// Atomically replace an agent's ordered rule list (≤100 rules).
    void replace_rules(const std::string& agent_id,
                       const std::vector<ApprovalRule>& rules);
    /// First matching rule → allow(true)/deny(false); nullopt = no match.
    std::optional<bool> match_rule(const std::string& agent_id,
                                   const std::string& kind,
                                   const std::string& payload);
    /// Record an auto decision as a resolved row (status auto_allowed /
    /// auto_denied) so the inbox shows what the rules did.
    void record_auto(const std::string& agent_id,
                     const std::string& agent_name,
                     const std::string& kind, const std::string& payload,
                     bool allowed);
    /// Shell-style glob: `*` = any run, `?` = one char. Pure; for tests.
    static bool glob_match(std::string_view pattern, std::string_view text);

private:
    std::filesystem::path db_path_;
    void ensure_schema();
    sqlite3* open_conn();
};
