#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace hwrunner {

// ----- container lifecycle (exposed for the v2 fleet manager, Phase B) ---

/// hive-<agent> with unsafe chars clamped to '-'.
std::string container_name(const std::string& agent);

/// ~/.local/share/hive-worker
std::string worker_dir();

/// First-run seeding of the shared worker workspace (dirs + index.md).
/// Idempotent and NON-destructive: creates missing layout dirs, writes
/// index.md ONLY when absent — agents maintain the live map thereafter.
void seed_workspace(const std::string& workspace_dir);

/// Quick synchronous podman call (no streaming); returns exit code.
int podman_simple(const std::vector<std::string>& args, std::string* out);

/// Ensure the agent's container exists + runs on THIS worker.
/// image defaults to the v1 image; the v2 fleet passes its own tag.
/// gpu (Phase 35): pass --device /dev/kfd --device /dev/dri
/// --group-add keep-groups so HIP/Vulkan see the host GPU.
void ensure_container(const std::string& agent,
                      std::string image = "localhost/hive-agent:latest",
                      bool gpu = false);


struct ExecOutcome {
    int exit_code = -1;
    bool timed_out = false;
    int64_t duration_ms = 0;
    std::string error;      // "" on success; infra errors (no podman etc.)
};

/// Ensure the agent's container exists + runs on THIS worker, then
/// `podman exec` argv inside it, streaming combined stdout/stderr via
/// on_chunk. env_content (may be "") is written to a 0600 tmpfs file and
/// passed as --env-file; deleted when the exec returns.
///
/// Security: argv is executed VERBATIM (execvp, no shell) — the worker
/// never interprets a command string from the wire.
ExecOutcome run_container_exec(const std::string& agent,
                               const std::vector<std::string>& argv,
                               const std::string& env_content,
                               int timeout_s,
                               const std::function<void(std::string_view)>& on_chunk);

struct ScreenResult {
    bool running = false;
    uint16_t port = 0;
    std::string password;
    std::string error;      // "" on success
};

/// Screen control for an agent's container ON THIS WORKER. Mirrors the
/// server's ContainerManager screen stack (Phase 24/25): the agent's
/// container is (re)created from the BROWSER image with 127.0.0.1::6080
/// published (websockify), then hive-screen.sh brings up the display
/// stack. image + resolution come from the server (it owns the config).
/// action: "status" | "start" | "stop".
/// `type` is the agent type ("" standard / "gpu") — the worker maps it to
/// the screen-capable image (:browser or :gpu), mirroring fleet's
/// resolved_image. The hub stays hardware-dumb.
ScreenResult screen_ctl(const std::string& agent, const std::string& action,
                        const std::string& type, const std::string& resolution);

} // namespace hwrunner
