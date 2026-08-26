#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>
#include <sqlite3.h>

/**
 * WorkerStore — Phase 30.1: hive-worker registry.
 *
 * A worker is a remote machine running the hive-worker binary: it dials
 * OUT to the server's /api/workers/channel WebSocket and executes agent
 * containers locally (rootless podman, same ContainerManager). The server
 * keeps roster, threads, credentials — the worker is a dumb executor.
 *
 * Enrollment is CLI-only (admin runs `hive-server enroll-worker <name>`
 * on the server): a raw `hmwk_...` key is printed once and only its
 * SHA-256 hash is stored here (same pattern as AuthStore API keys).
 * The worker presents the key on its WebSocket hello; validate()
 * authenticates and refreshes last_seen/caps in one shot.
 *
 * DB file: ~/.local/share/hivemind/workers.db (fresh connection per op).
 */
struct WorkerInfo {
    std::string name;       // [a-z0-9-], primary key
    std::string caps;       // JSON text from the hello frame (gpu, arch…)
    int proto = 1;          // protocol major from hello
    std::string last_seen;  // ISO timestamp of last validated connect
    std::string created_at;
};

class WorkerStore {
public:
    WorkerStore();                                    // default XDG path
    explicit WorkerStore(std::filesystem::path db);   // tests
    ~WorkerStore();

    /// Enroll a worker. Returns the RAW key (print once, never stored).
    /// Throws std::invalid_argument on bad/duplicate name.
    std::string enroll(const std::string& name);

    /// Validate a raw key. On success updates last_seen (+caps/proto when
    /// non-empty) and returns the worker; nullopt when unknown/revoked.
    std::optional<WorkerInfo> validate(const std::string& raw_key,
                                       const std::string& caps = "",
                                       int proto = 1);

    std::optional<WorkerInfo> get(const std::string& name);
    std::vector<WorkerInfo> list();

    /// Revoke: deletes the row (key dies with it). False when unknown.
    bool remove(const std::string& name);

    /// [a-z0-9-], 1-32 chars.
    static bool valid_name(const std::string& n);

private:
    std::filesystem::path db_path_;
    void ensure_schema();
    sqlite3* open_conn();
};
