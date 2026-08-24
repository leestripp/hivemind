#pragma once

class ExecAuditStore;

#include <optional>
#include <string>
#include <filesystem>

/**
 * ContainerManager — rootless-podman sandbox lifecycle for roster agents
 * (HiveMind 2.0, Plan.md Phase 2).
 *
 * Wraps the podman CLI (no libpod dependency). One container per agent,
 * named hive-<agent_id>. Company workspace model:
 *   /workspace  — shared named volume "hive-workspace" (all agents see it)
 *   /home/hive  — per-agent bind mount (~/.local/share/hivemind/agents/<id>/home)
 *
 * The manager never falls back to host execution — if podman or the
 * container is unavailable, calls fail with a clear error.
 */
namespace hivemind { class SecretStore; }

class ContainerManager {
public:
    struct ExecResult {
        int exit_code = -1;
        std::string output;      // stdout + stderr merged
        bool timed_out = false;
    };

    struct Status {
        bool exists = false;
        bool running = false;
        std::string container;   // hive-<agent_id>
        std::string image;
        std::string workspace;   // host folder mounted at /workspace
    };

    /// True if a podman binary is on PATH (does not prove it can run
    /// containers — nested/restricted sandboxes may still fail at run time).
    static bool podman_available();

    /// Derive the container name for an agent (pure; exposed for tests).
    static std::string container_name(const std::string& agent_id);

    /// Shell-quote a single argument (single-quote style). Pure; for tests.
    static std::string shell_quote(const std::string& arg);

    /// Build localhost/hive-agent:latest from the shipped Containerfile
    /// if it isn't present. Throws std::runtime_error on build failure.
    void ensure_image();

    /// Create (if needed) and start the agent's container; returns the
    /// container name. Idempotent. Throws std::runtime_error on failure.
    std::string ensure_running(const std::string& agent_id);

    Status status(const std::string& agent_id);
    bool stop(const std::string& agent_id);
    bool remove(const std::string& agent_id);

    /// Run a command inside the agent's container as user `hive` in
    /// /workspace via `bash -lc`. timeout_s clamps via coreutils `timeout`.
    ExecResult exec(const std::string& agent_id, const std::string& command,
                    int timeout_s = 120);

    /// Per-agent home directory on the host (bind-mounted to /home/hive).
    static std::filesystem::path agent_home_dir(const std::string& agent_id);

    // ── Phase 6: computer use (screen pipeline) ──────────────────────
    /// Build localhost/hive-agent:browser from Containerfile.hive-agent-browser
    /// if missing. Throws std::runtime_error on build failure.
    void ensure_browser_image();
    /// True when the browser image exists (cheap `podman image exists`).
    bool browser_image_present();
    /// Host-loopback port the agent's container publishes 6080 on
    /// (`podman port <name> 6080`); 0 when not published/not running.
    uint16_t screen_port(const std::string& agent_id);
    /// Ensure the agent's container runs the browser image with 6080
    /// published on 127.0.0.1 and the display stack (Xvfb/x11vnc/
    /// websockify --web /usr/share/novnc) up. Recreates the container when
    /// it predates port publishing. Returns the host port. Throws on failure.
    uint16_t ensure_screen(const std::string& agent_id);
    /// Stop the display stack (container keeps running). False if not found.
    bool stop_screen(const std::string& agent_id);

private:
    /// Phase 15 secret broker: granted secrets ride into the container via
    /// `podman exec --env-file` (0600 tmp file, deleted after) — values
    /// never appear in podman's argv, bash history, or the LLM context.
    static hivemind::SecretStore* secrets();   // function-local static (impl)
public:
    /// Process-wide SecretStore (same instance exec() injects from).
    /// Public for tools that gate on grants (BrowserFillSecretTool).
    static hivemind::SecretStore& secret_store() { return *secrets(); }

    /// Container exec audit trail (Phase 8). Logged from exec().
    static ExecAuditStore& exec_audit();
private:
    /// Run a podman command line (already quoted), capture output + rc.
    static ExecResult run(const std::string& cmdline, int timeout_s = 0);

    static constexpr const char* kImage = "localhost/hive-agent:latest";
    static constexpr const char* kBrowserImage = "localhost/hive-agent:browser";
};

/// Process-wide shared instance.
ContainerManager& container_manager();
