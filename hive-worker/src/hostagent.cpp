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

bool require_root() {
    if (geteuid() == 0) return true;
    std::cerr << "must run as root: sudo hive-worker <subcommand> ...\n";
    return false;
}

} // namespace

int provision(const std::string& agent_name,
              const std::string& worker_user,
              const std::string& binary_path) {
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
        "/usr/local/bin/hive-agent\n";
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
