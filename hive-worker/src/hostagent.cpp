// =============================================================================
// src/hostagent.cpp — host-agent provisioning (Phase 72, Plan.md "Host Agents")
//
// A host agent runs BARE-METAL as unix user hivex-<name> with passwordless
// sudo — NO sandbox. Provisioning is deliberately a LOCAL, MANUAL, root-only
// operation: nothing reachable via the hub API can create unix users or
// sudoers entries.
// =============================================================================
#include "hostagent.h"
#include "runner.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <cstring>
#include <iostream>
#include <string>
#include <sys/types.h>
#include <unistd.h>
#include <vector>

namespace hwhostagent {
namespace {

constexpr const char* kWarning = R"(
╔══════════════════════════════════════════════════════════════════════╗
║  ⚠  HOST AGENT — NO SANDBOX                                          ║
║                                                                      ║
║  This agent runs DIRECTLY on this machine as a real user with        ║
║  passwordless sudo. It can install software, modify system files,    ║
║  and — through error, hallucination, or prompt injection —           ║
║  DESTROY this machine.                                               ║
║                                                                      ║
║  Only do this on a machine you don't mind losing.                    ║
╚══════════════════════════════════════════════════════════════════════╝
)";

/// Simple synchronous run, argv verbatim, no shell. Returns exit code.
int run(const std::vector<std::string>& args) {
    std::string cmd;
    for (const auto& a : args) {
        cmd += " '";
        for (char c : a) { if (c == '\'') cmd += "'\\''"; else cmd += c; }
        cmd += "'";
    }
    cmd += " 2>&1";
    FILE* p = popen(cmd.c_str(), "r");
    if (!p) return -1;
    char buf[512];
    while (fgets(buf, sizeof buf, p))
        std::cerr << "  " << buf;
    int rc = pclose(p);
    return WIFEXITED(rc) ? WEXITSTATUS(rc) : -1;
}

/// Red warning + type-the-hostname confirmation. Returns false on decline.
bool confirm() {
    char hn[256] = {0};
    if (gethostname(hn, sizeof hn - 1) != 0) std::strcpy(hn, "this-machine");
    std::cerr << kWarning << "\nTo proceed, type this machine's hostname ("
              << hn << "): ";
    std::cout.flush();
    std::string typed;
    if (!std::getline(std::cin, typed) || typed != hn) {
        std::cerr << "Aborted — hostname did not match.\n";
        return false;
    }
    return true;
}

/// Root helper installed by provision: removes a host-agent unix user
/// (processes, home — archived first — and its sudoers entry). The
/// workspace under --host-workspace-root is NEVER touched. The worker
/// invokes this via `sudo -n` on roster delete (auto-cleanup); the
/// sudoers grant is limited to exactly this root-owned, validated script.
constexpr const char* kHelper = R"SH(#!/bin/sh
# HiveMind Phase 72: remove one host-agent unix user. Root-only via
# /etc/sudoers.d/hivex-deprovision. NEVER touches the agent workspace
# (<host-workspace-root>/hivex-<name>) — that drive is permanent.
set -eu
user="${1:-}"
echo "$user" | grep -Eq '^hivex-[a-z0-9-]{1,26}$' \
    || { echo "bad user name: '$user'" >&2; exit 2; }
id "$user" >/dev/null 2>&1 || { echo "no such user: $user" >&2; exit 1; }
pkill -u "$user" 2>/dev/null || true
stamp=$(date -u +%Y%m%d-%H%M%S)
if [ -d "/home/$user" ]; then
    tar -czf "/var/backups/$user-$stamp.tar.gz" -C /home "$user" \
        || echo "warning: home archive failed" >&2
fi
userdel -r "$user" 2>/dev/null || userdel "$user"
rm  "/etc/sudoers.d/$user"
echo "hivex-deprovision: removed $user (home archived to /var/backups/$user-$stamp.tar.gz; workspace untouched)"
)SH";

/// Root helper installed by install_support: provisions ONE host-agent
/// unix user (useradd, locked password, per-user sudoers, binary install).
/// The worker calls it via `sudo -n` when a host agent spawns
/// unprovisioned — the grant covers exactly this validated script.
/// Args: <hivex-user> <worker-user> <hive-agent-binary-src> [workspace-root]
constexpr const char* kProvisionHelper = R"SH(#!/bin/sh
# HiveMind Phase 72: auto-provision one host-agent unix user. Root-only
# via /etc/sudoers.d/hivex-helpers. When a workspace root is given, maps
# the share-transport dirs into it and grants the agent ACLs (only).
set -eu
user="${1:-}"; wuser="${2:-}"; src="${3:-}"
echo "$user" | grep -Eq '^hivex-[a-z0-9-]{1,26}$' \
    || { echo "bad user name: '$user'" >&2; exit 2; }
[ -n "$wuser" ] && [ "$wuser" != "root" ] \
    || { echo "bad worker user: '$wuser'" >&2; exit 2; }
[ -f "$src" ] || { echo "hive-agent binary missing: $src" >&2; exit 2; }
home="/home/$user"
if ! id "$user" >/dev/null 2>&1; then
    /usr/sbin/useradd -m -s /bin/bash -c "HiveMind host agent" "$user"
    /usr/bin/passwd -l "$user" >/dev/null 2>&1 || true
fi
/usr/bin/install -m 0755 -o root -g root "$src" /usr/local/bin/hive-agent
# Bounded-exec helper for the worker's hub exec op (Phase 72.1) — the
# sudoers entry below whitelists it, replacing the Phase 72 approach of
# wrapping commands in timeout(1)/env(1) (matched no grant; exec always
# failed "interactive authentication is required").
helper="/tmp/hivex-exec-helper-$$"
cat > "$helper" <<'EOS'
#!/bin/sh
# HiveMind Phase 72.1: bounded exec as this hivex user for the worker's
# hub "exec" op. argv: <timeout_s> <argv...> — env(1) wrappers for env
# content belong in <argv...> (env is a normal command here).
set -eu
t="${1:-120}"; [ $# -ge 2 ] || { echo "usage: hivex-exec <timeout_s> <argv...>" >&2; exit 2; }
shift
exec /usr/bin/timeout -s TERM --kill-after=5 "$t" "$@"
EOS
/usr/bin/install -m 0755 -o root -g root "$helper" /usr/local/sbin/hivex-exec
rm -f "$helper"
tmp="/tmp/hivex-prov-sudoers-$$"
{
    echo "# HiveMind Phase 72 host agent - auto-provisioned by hivex-provision."
    echo "# UNSANDBOXED: this user is effectively root. Machine = expendable."
    echo "$user ALL=(ALL) NOPASSWD: ALL"
    echo "$wuser ALL=($user) NOPASSWD: /usr/bin/tee $home/.hive-env, /usr/bin/chmod 600 $home/.hive-env, /usr/local/bin/hive-agent, /usr/local/sbin/hivex-exec, /usr/local/bin/hive-screen.sh"
} > "$tmp"
/usr/sbin/visudo -c -f "$tmp" >/dev/null
/usr/bin/install -m 0440 -o root -g root "$tmp" "/etc/sudoers.d/$user"
rm -f "$tmp"
# Workspace-share mapping (2026-10-12 QueenBee incident): share_file
# staging lives in the worker's private staging workspace, which
# containers bind-mount as /workspace. Host agents run in the REAL
# workspace — map incoming/ + shared/ to the staging dirs and grant the
# agent rwx on the workspace plus traverse-only on the staging path
# under the worker user's home (home itself stays unreadable).
# attachments/ is mapped too (2026-10-12): user->agent chat attachments
# land in staging attachments/<tid>/ and the agent reads them at
# <ws>/attachments/<tid>/.
wsroot="${4:-}"
ws="/home/$wuser/.local/share/hive-worker/workspace"
if [ -n "$wsroot" ] && [ -d "$wsroot" ] && [ -d "$ws" ]; then
    /usr/bin/install -d -m 0775 -o "$wuser" -g "$wuser" \
        "$ws/incoming" "$ws/shared" "$ws/attachments"
    for d in incoming shared attachments; do
        if [ ! -e "$wsroot/$d" ] && [ ! -L "$wsroot/$d" ]; then
            /usr/bin/ln -s "$ws/$d" "$wsroot/$d"
        fi
    done
    /usr/bin/setfacl -m "u:$user:rwx" -d -m "u:$user:rwx" "$wsroot"
    # 2026-10-13 Bob/Forge attach incident: subdirs created BEFORE this
    # agent's default grant (attachments/, departments/, scratch/) never
    # inherit it — attach_file stages at <wsroot>/attachments/<tid>/
    # outgoing and EACCESed for every host agent but the first. Recurse
    # rwX over the existing tree and seed defaults on its dirs.
    /usr/bin/setfacl -R   -m "u:$user:rwX" "$wsroot"
    /usr/bin/setfacl -R -d -m "u:$user:rwx" "$wsroot"
    # Agent state dir root: owned by whichever agent first created it,
    # so later agents could not mkdir their own (2026-10-13 spawn
    # crash). Root-own it and ACL-grant each provisioned agent rwx.
    [ -d "$wsroot/.hive-agent" ] || \
        /usr/bin/install -d -m 0755 -o root -g root "$wsroot/.hive-agent"
    /usr/bin/setfacl -m "u:$user:rwx" -d -m "u:$user:rwx" \
        "$wsroot/.hive-agent"
    /usr/bin/setfacl -m "u:$user:x" "/home/$wuser" \
        "/home/$wuser/.local" "/home/$wuser/.local/share" \
        "/home/$wuser/.local/share/hive-worker"
fi
echo "hivex-provision: $user ready (sudo NOPASSWD, /usr/local/bin/hive-agent)"
)SH";

bool require_root() {
    if (geteuid() == 0) return true;
    std::cerr << "must run as root: sudo hive-worker <subcommand> ...\n";
    return false;
}

} // namespace

int provision(const std::string& agent_name,
              const std::string& worker_user,
              const std::string& binary_path,
              const std::string& workspace_root) {
    if (!require_root()) return 2;
    const std::string user = hwrunner::host_user(agent_name);
    if (worker_user.empty() || worker_user == "root") {
        std::cerr << "worker user unknown (SUDO_USER unset / root) — pass "
                     "--worker-user <account hive-worker runs as>\n";
        return 2;
    }
    if (binary_path.empty() || access(binary_path.c_str(), X_OK) != 0) {
        std::cerr << "hive-agent binary not found/executable: '"
                  << binary_path << "' — pass --binary <path>\n";
        return 2;
    }
    if (!confirm()) return 1;

    // 1. User (idempotent: useradd fails if it exists — that's fine).
    if (run({"/usr/sbin/useradd", "-m", "-s", "/bin/bash",
             "-c", "HiveMind host agent " + agent_name, user}) != 0 &&
        !hwrunner::host_agent_provisioned(agent_name)) {
        std::cerr << "useradd failed for " << user << "\n";
        return 1;
    }
    // Lock the password — login is never needed, sudo NOPASSWD covers use.
    if (run({"/usr/bin/passwd", "-l", user}) != 0)
        std::cerr << "warning: passwd -l failed (continuing)\n";

    // 2. Binary — shared by all host agents on this rig.
    if (run({"/usr/bin/install", "-m", "0755", "-o", "root", "-g", "root",
             binary_path, "/usr/local/bin/hive-agent"}) != 0) {
        std::cerr << "binary install failed\n";
        return 1;
    }

    // 3. Sudoers — TWO rules:
    //    a) the agent itself: full NOPASSWD sudo (that is the feature).
    //    b) the worker user: launch + env-staging verbs as hivex-<name>.
    //       tee/chmod stage the 0600 env file into the agent's home so
    //       secrets never appear in argv (ps is world-readable).
    const std::string sfile = "/etc/sudoers.d/hivex-" + user.substr(6);
    const std::string home = "/home/" + user;
    const std::string content =
        "# HiveMind Phase 72 host agent — created by hive-worker "
        "provision-host-agent.\n"
        "# UNSANDBOXED: this user is effectively root. Machine = expendable.\n"
        + user + " ALL=(ALL) NOPASSWD: ALL\n"
        // No CWD= tag and no -D in the spawn: sudo-rs (in use on Ubuntu
        // rigs) rejects --chdir entirely. hive-agent chdirs itself.
        + worker_user + " ALL=(" + user + ") NOPASSWD: "
        "/usr/bin/tee " + home + "/.hive-env, "
        "/usr/bin/chmod 600 " + home + "/.hive-env, "
        "/usr/local/bin/hive-agent, /usr/local/bin/hive-screen.sh\n";
    {
        // Write to a temp file, visudo-check, THEN install — a broken
        // sudoers.d entry can lock out sudo on the whole box.
        const std::string tmp = std::string("/tmp/hivex-sudoers-XXXXXX");
        std::string tmpl = tmp;
        int fd = mkstemp(tmpl.data());
        if (fd < 0) { std::cerr << "mkstemp failed\n"; return 1; }
        FILE* f = fdopen(fd, "w");
        fputs(content.c_str(), f);
        fclose(f);
        if (run({"/usr/sbin/visudo", "-c", "-f", tmpl}) != 0) {
            std::cerr << "visudo rejected the generated rules — NOT "
                         "installing anything\n";
            unlink(tmpl.c_str());
            return 1;
        }
        if (run({"/usr/bin/install", "-m", "0440", "-o", "root", "-g",
                 "root", tmpl, sfile}) != 0) {
            unlink(tmpl.c_str());
            return 1;
        }
        unlink(tmpl.c_str());
    }

    // 4. Workspace — the SHARED drive every host agent runs in (the
    //    same /workspace the containers see), NEVER deleted. The worker
    //    must be started with --host-workspace-root pointing at the same
    //    path; provision just verifies the mount exists.
    if (!workspace_root.empty()) {
        if (!std::filesystem::is_directory(workspace_root)) {
            std::cerr << "workspace root missing: " << workspace_root
                      << "\n";
            return 1;
        }
        std::cerr << "  workspace: " << workspace_root
                  << " (SHARED by all host agents — never cleaned; set "
                     "--host-workspace-root on the worker to match)\n";

        // 5. Share-transport mapping (2026-10-12 QueenBee incident):
        //    share_file stages into the worker's private staging
        //    workspace (~/.local/share/hive-worker/workspace), which
        //    containers bind-mount as /workspace. Host agents run in
        //    the REAL workspace — map incoming/ + shared/ to the
        //    staging dirs, grant rwx on the workspace, and traverse-
        //    only on the staging path under the worker user's home.
        //    attachments/ joined the mapping 2026-10-12 (user->agent
        //    chat attachments land in staging attachments/<tid>/).
        const std::string hwbase =
            "/home/" + worker_user + "/.local/share/hive-worker";
        const std::string staging = hwbase + "/workspace";
        if (std::filesystem::is_directory(staging)) {
            run({"/usr/bin/install", "-d", "-m", "0775", "-o",
                 worker_user, "-g", worker_user,
                 staging + "/incoming", staging + "/shared",
                 staging + "/attachments"});
            for (const char* d : {"incoming", "shared", "attachments"}) {
                const std::string p = workspace_root + "/" + d;
                std::error_code ec;
                if (!std::filesystem::exists(p, ec) &&
                    !std::filesystem::is_symlink(p, ec))
                    run({"/usr/bin/ln", "-s", staging + "/" + d, p});
            }
            run({"/usr/bin/setfacl", "-m", "u:" + user + ":rwx",
                 "-d", "-m", "u:" + user + ":rwx", workspace_root});
            // 2026-10-13 Bob/Forge attach incident: subdirs created
            // BEFORE this agent's default grant (attachments/,
            // departments/, scratch/) never inherit it — attach_file
            // stages at <root>/attachments/<tid>/outgoing and EACCESed
            // for every host agent but the first. Recurse rwX over the
            // existing tree and seed defaults on its dirs.
            run({"/usr/bin/setfacl", "-R", "-m", "u:" + user + ":rwX",
                 workspace_root});
            run({"/usr/bin/setfacl", "-R", "-d", "-m",
                 "u:" + user + ":rwx", workspace_root});
            run({"/usr/bin/setfacl", "-m", "u:" + user + ":x",
                 "/home/" + worker_user, "/home/" + worker_user +
                 "/.local", "/home/" + worker_user + "/.local/share",
                 hwbase});
        }
    }

    // 5. Rig-level support: root helpers + the worker's sudoers grant —
    //    enables auto-provision at spawn AND auto-deprovision on delete.
    if (install_support(worker_user) != 0) return 1;

    std::cerr << "\nHost agent '" << agent_name << "' provisioned:\n"
              << "  user:    " << user << " (home " << home
              << ", password locked)\n"
              << "  sudo:    " << sfile << " (NOPASSWD: ALL)\n"
              << "  binary:  /usr/local/bin/hive-agent\n"
              << "  worker:  " << worker_user << " may launch/stage as "
              << user << "\n\n"
              << "Remember: this worker must also run with "
              << "--allow-host-agents (or HIVE_ALLOW_HOST_AGENTS=1) or "
              << "spawns are refused.\n";
    return 0;
}

int install_support(const std::string& worker_user) {
    if (!require_root()) return 2;
    if (worker_user.empty() || worker_user == "root") {
        std::cerr << "worker user unknown — pass --worker-user "
                     "<account hive-worker runs as>\n";
        return 2;
    }
    // Both helpers are root-owned, argument-validating scripts; the
    // grant below covers EXACTLY these two paths and nothing else.
    const struct { const char* path; const char* body; } helpers[] = {
        {"/usr/local/sbin/hivex-provision",   kProvisionHelper},
        {"/usr/local/sbin/hivex-deprovision", kHelper},
    };
    for (const auto& h : helpers) {
        FILE* f = fopen(h.path, "w");
        if (!f) { std::cerr << "cannot write " << h.path << "\n"; return 1; }
        fputs(h.body, f);
        fclose(f);
        run({"/usr/bin/chmod", "0755", h.path});
        run({"/usr/bin/chown", "root:root", h.path});
    }
    const std::string grant =
        "# HiveMind Phase 72: the worker may auto-provision host agents\n"
        "# at spawn and auto-deprovision them on roster delete. Both\n"
        "# scripts are root-owned and validate their own args.\n"
        + worker_user + " ALL=(root) NOPASSWD: "
        "/usr/local/sbin/hivex-provision, "
        "/usr/local/sbin/hivex-deprovision\n";
    std::string tmpl = "/tmp/hivex-helpers-XXXXXX";
    const int fd = mkstemp(tmpl.data());
    if (fd < 0) { std::cerr << "mkstemp failed\n"; return 1; }
    FILE* gf = fdopen(fd, "w");
    fputs(grant.c_str(), gf);
    fclose(gf);
    if (run({"/usr/sbin/visudo", "-c", "-f", tmpl}) != 0) {
        std::cerr << "visudo rejected the helper grant — NOT installing\n";
        unlink(tmpl.c_str());
        return 1;
    }
    if (run({"/usr/bin/install", "-m", "0440", "-o", "root", "-g", "root",
             tmpl, "/etc/sudoers.d/hivex-helpers"}) != 0) {
        unlink(tmpl.c_str());
        return 1;
    }
    unlink(tmpl.c_str());
    // Superseded by hivex-helpers (older installs only granted the
    // deprovision script).
    unlink("/etc/sudoers.d/hivex-deprovision");
    // Phase 72.2: screen-stack scripts for host agents (noVNC + shared
    // browser). Installed from the repo data dir when run from the
    // repository root; best-effort — a missing tree just skips them.
    for (const char* s :
         {"hive-screen.sh", "hive-chrome.sh", "browser_driver.py"}) {
        const std::string src = std::string("hive-server/data/") + s;
        if (std::filesystem::exists(src))
            run({"/usr/bin/install", "-m", "0755", "-o", "root", "-g",
                 "root", src, std::string("/usr/local/bin/") + s});
    }
    std::cerr << "Host-agent support installed:\n"
              << "  helpers: /usr/local/sbin/hivex-provision, "
                 "/usr/local/sbin/hivex-deprovision (root:root 0755)\n"
              << "  grant:   /etc/sudoers.d/hivex-helpers ("
              << worker_user << " may run exactly those as root)\n"
              << "Host agents now auto-provision at spawn and "
                 "auto-deprovision on roster delete.\n";
    return 0;
}

int deprovision(const std::string& agent_name) {
    if (!require_root()) return 2;
    const std::string user = hwrunner::host_user(agent_name);
    if (!hwrunner::host_agent_provisioned(agent_name)) {
        std::cerr << "no such user: " << user << "\n";
        return 1;
    }
    if (!confirm()) return 1;

    // Stop anything the agent is running, archive the home (never
    // silently destroy the agent's work), remove user + sudoers.
    run({"/usr/bin/pkill", "-u", user});   // best-effort
    char stamp[32] = {0};
    {
        time_t t = time(nullptr);
        strftime(stamp, sizeof stamp, "%Y%m%d-%H%M%S", gmtime(&t));
    }
    const std::string arc = "/var/backups/" + user + "-" + stamp + ".tar.gz";
    if (run({"/usr/bin/tar", "-czf", arc, "-C", "/home", user}) == 0)
        std::cerr << "home archived to " << arc << "\n";
    else
        std::cerr << "warning: home archive failed (continuing)\n";
    if (run({"/usr/sbin/userdel", "-r", user}) != 0) {
        std::cerr << "userdel failed\n";
        return 1;
    }
    run({"/usr/bin/rm", "-f",
         "/etc/sudoers.d/hivex-" + user.substr(6)});
    std::cerr << "Host agent '" << agent_name << "' deprovisioned "
              << "(user " << user << " removed, sudoers entry deleted, "
              << "home archived). /usr/local/bin/hive-agent left in place "
              << "— it may serve other host agents.\n";
    return 0;
}

} // namespace hwhostagent
