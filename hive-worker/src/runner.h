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
/// memory/cpus (Phase 69): per-agent resource caps from the hub's spawn
/// frame; ""/0 = keep the pinned (or factory) defaults — 4g/2. When
/// passed explicitly they are PINNED to disk so every recreate path
/// (including screen_ctl) reuses them. A live container whose limits
/// differ is reconciled IN PLACE via `podman update` — no recreate.
// ── Agent service-port publishing (2026-10-09) ──────────────────────
// Container-side base of the per-agent public service range: ports
// 8000..8000+N-1 in the container appear on the host at
// service_port_base(agent)..+N-1 (see runner.cpp for the full contract).
inline constexpr int kServiceContainerBase = 8000;

/// Allocated host base port for the agent's service range (persisted in
/// <worker_dir>/service_ports.txt; 0 when the feature is disabled).
int service_port_base(const std::string& agent);

/// Address advertised to agents/clients for published services
/// (HIVE_SERVICE_HOST override, else first global non-loopback IPv4).
std::string service_host_addr();

/// Run /workspace/.hivemind/boot.sh detached when present — called after
/// every container (re)create+start so agent services self-recover.
void maybe_run_boot_hook(const std::string& agent);

void ensure_container(const std::string& agent,
                      std::string image = "localhost/hive-agent:latest",
                      bool gpu = false,
                      const std::string& memory = "",
                      double cpus = 0);

// ── Phase 72: host agents (bare-metal, sudo, NO sandbox) ─────────────
/// Worker-level opt-in for host agents — set from --allow-host-agents /
/// HIVE_ALLOW_HOST_AGENTS=1 in main.cpp BEFORE the WS connect. Default
/// false: a hub frame alone can never conjure a host agent on a rig
/// whose operator didn't opt in.
extern bool g_allow_host_agents;

/// The dedicated unix user a host agent runs as: "hivex-" + lowercased
/// name, unsafe chars clamped to '-', 32-char username cap.
std::string host_user(const std::string& agent);

/// True when hivex-<name> exists (someone ran provision-host-agent).
bool host_agent_provisioned(const std::string& agent);

/// Shared workspace for host agents — set from --host-workspace-root /
/// HIVE_HOST_WORKSPACE_ROOT in main.cpp. Empty = the agent's home dir.
/// This IS the workspace (e.g. /workspace on the host): the SAME shared
/// company drive every agent sees, exactly like the containers'
/// bind-mounted /workspace. NEVER deleted by deprovision/remove.
extern std::string g_host_workspace_root;

/// Where a host agent runs (its cwd / project root): the shared
/// g_host_workspace_root when set, else /home/hivex-<name>.
std::string host_workspace(const std::string& agent);

/// Quick synchronous `sudo -n -H -u <user> <args...>` — no shell,
/// stdin optionally redirected from stdin_path, combined stdout/stderr
/// captured into out. Returns the exit code (-1 = infra failure).
int run_as_user(const std::string& user,
                const std::vector<std::string>& args,
                const std::string& stdin_path, std::string* out);

/// Phase 69: parse a podman size string ("4g", "1536m", "512") to bytes.
/// Returns -1 on garbage. Exposed for tests.
long long parse_memory_bytes(const std::string& s);

/// Phase 69: format a cpu count the way podman --cpus wants it
/// ("12", "1.5"). Exposed for tests.
std::string format_cpus(double cpus);


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
/// Pinned screen image for an agent ("" when not screen-pinned). The
/// marker means "this agent HAS a screen session" — spawns auto-restore
/// the stack after a container recreate (Phase 66f).
std::string screen_pinned_image(const std::string& agent);
ScreenResult screen_ctl(const std::string& agent, const std::string& action,
                        const std::string& type, const std::string& resolution);

} // namespace hwrunner
