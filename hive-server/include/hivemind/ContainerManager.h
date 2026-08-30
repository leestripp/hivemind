#pragma once

class ExecAuditStore;

#include <string>

/**
 * ContainerManager (v2 statics-only).
 *
 * Workers own podman now (hive-worker/fleet.cpp) - everything that used to
 * run containers on the server host is gone. What remains are the process-
 * wide helpers other server components share:
 *   - secret_store()   SecretStore for channel exec env injection
 *   - exec_audit()     admin exec audit trail
 *   - browser_image_name()  screen_ctl image name (worker recreates with it)
 *   - shell_quote()    pure helper (ConnectorRoutes)
 */
namespace hivemind { class SecretStore; }

class ContainerManager {
public:
    struct ExecResult {
        int exit_code = -1;
        std::string output;      // stdout + stderr merged
        bool timed_out = false;
    };

    /// Shell-quote a single argument (single-quote style). Pure.
    static std::string shell_quote(const std::string& arg);

private:
    static hivemind::SecretStore* secrets();   // function-local static (impl)
public:
    /// Process-wide SecretStore (channel exec env injection).
    static hivemind::SecretStore& secret_store() { return *secrets(); }

    /// Exec audit trail (channel execs log here via WorkerRouter).
    static ExecAuditStore& exec_audit();

    /// Browser/screen image name (workers recreate screen containers with it).
    static const char* browser_image_name() { return kBrowserImage; }

private:
    static constexpr const char* kBrowserImage = "localhost/hive-agent:browser";
};
