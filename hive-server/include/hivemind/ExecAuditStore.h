#pragma once

#include <filesystem>
#include <string>
#include <vector>

struct sqlite3;

/**
 * ExecAuditStore — Phase 8 leftover: audit log of every command executed
 * inside agent containers (the ContainerManager::exec choke point covers
 * container_exec, browser_* tools, and screen-stack management).
 *
 * Doubles as secrets-broker accountability (Phase 15): commands are logged
 * verbatim, which is safe because secret VALUES never appear in argv — they
 * ride in via --env-file (HIVE_SECRET_<NAME>) and are deleted immediately.
 *
 * Long commands are truncated to 500 chars. Newest-first listing.
 */
class ExecAuditStore {
public:
    struct Entry {
        int64_t id = 0;
        std::string ts;            // UTC ISO-8601
        std::string agent_id;
        std::string command;       // truncated to 500 chars
        int exit_code = 0;
        bool timed_out = false;
        int64_t duration_ms = 0;
    };

    explicit ExecAuditStore(std::filesystem::path db_path);
    ~ExecAuditStore() = default;

    void log(const std::string& agent_id, const std::string& command,
             int exit_code, bool timed_out, int64_t duration_ms);

    /// Newest first. agent_id empty → all agents.
    std::vector<Entry> list(const std::string& agent_id = "", int limit = 100);

    static constexpr size_t kCommandLimit = 500;

private:
    std::filesystem::path db_path_;
    sqlite3* open_conn();
};
