#pragma once

#include <string>

// ── Phase 72: host-agent provisioning (Plan.md "Host Agents") ────────
// These run as `sudo hive-worker provision-host-agent <name>` ON the rig
// — deliberately manual and local so nothing reachable via the hub API
// can ever mint a sudo user remotely.
namespace hwhostagent {

/// Create the hivex-<name> user (locked password, dedicated home),
/// install the hive-agent binary to /usr/local/bin, and write the
/// sudoers rules (validated with visudo -c). Requires root. The red
/// warning + hostname type-to-confirm run BEFORE any change.
/// worker_user: the account hive-worker runs as (defaults to
/// $SUDO_USER); binary: path to the hive-agent binary to install
/// (default: alongside this executable).
/// Returns 0 on success; prints diagnostics to stderr.
int provision(const std::string& agent_name,
              const std::string& worker_user,
              const std::string& binary_path,
              const std::string& workspace_root = "");

/// Once-per-rig ROOT setup: install the two /usr/local/sbin/hivex-*
/// helper scripts + a single /etc/sudoers.d/hivex-helpers grant letting
/// the WORKER user run exactly those scripts as root. After this, agent
/// create/delete through the hub auto-provisions/deprovisions — the
/// rig-level opt-in that keeps per-agent operations self-service.
int install_support(const std::string& worker_user);

/// Stop the agent's processes, archive the home to
/// /var/backups/hivex-<name>-<date>.tar.gz, remove the user and its
/// sudoers file. Requires root; same hostname confirmation.
int deprovision(const std::string& agent_name);

} // namespace hwhostagent
