// =============================================================================
// src/ContainerManager.cpp — podman sandbox lifecycle (HiveMind 2.0, Phase 2)
// =============================================================================
#include "hivemind/ContainerManager.h"
#include "hivemind/SecretStore.h"
#include "hivemind/ServerConfig.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

namespace fs = std::filesystem;

ContainerManager& container_manager() {
    static ContainerManager instance;
    return instance;
}

// ─── helpers ────────────────────────────────────────────────────────────────

bool ContainerManager::podman_available() {
    static const bool available = [] {
        int rc = std::system("command -v podman >/dev/null 2>&1");
        return rc == 0;
    }();
    return available;
}

std::string ContainerManager::container_name(const std::string& agent_id) {
    return "hive-" + agent_id;
}

std::string ContainerManager::shell_quote(const std::string& arg) {
    std::string out = "'";
    for (char c : arg) {
        if (c == '\'') out += "'\\''";
        else out += c;
    }
    out += "'";
    return out;
}

fs::path ContainerManager::agent_home_dir(const std::string& agent_id) {
    return fs::path{std::getenv("HOME") ? std::getenv("HOME") : "."}
         / ".local" / "share" / "hivemind" / "agents" / agent_id / "home";
}

ContainerManager::ExecResult ContainerManager::run(const std::string& cmdline,
                                                   int timeout_s) {
    std::string full = cmdline;
    if (timeout_s > 0)
        full = "timeout " + std::to_string(timeout_s) + " " + cmdline;
    full += " 2>&1";

    ExecResult res;
    FILE* pipe = popen(full.c_str(), "r");
    if (!pipe) {
        res.output = "popen failed";
        return res;
    }
    char buf[4096];
    while (std::fgets(buf, sizeof(buf), pipe)) {
        res.output += buf;
        if (res.output.size() > 1 << 20) {  // 1 MiB cap
            res.output += "\n[output truncated]";
            break;
        }
    }
    int rc = pclose(pipe);
    if (WIFEXITED(rc)) {
        res.exit_code = WEXITSTATUS(rc);
        // coreutils timeout exits 124 on expiry
        res.timed_out = (timeout_s > 0 && res.exit_code == 124);
    } else {
        res.exit_code = -1;
    }
    return res;
}

// ─── image ──────────────────────────────────────────────────────────────────

void ContainerManager::ensure_image() {
    if (!podman_available())
        throw std::runtime_error("podman is not installed on this machine");

    auto r = run(std::string{"podman image exists "} + kImage);
    if (r.exit_code == 0) return;

    // Containerfile ships in the repo; server runs from the repo root
    // (same convention as web/dist static serving).
    const char* containerfile = "hive-server/data/Containerfile.hive-agent";
    if (!fs::exists(containerfile))
        throw std::runtime_error(
            std::string{"Containerfile not found at "} + containerfile +
            " (run hive-server from the repository root)");

    auto build = run(std::string{"podman build -t "} + kImage +
                         " -f hive-server/data/Containerfile.hive-agent"
                         " hive-server/data",
                     /*timeout_s=*/1800);
    if (build.exit_code != 0)
        throw std::runtime_error("podman build failed: " + build.output);
}

// ─── lifecycle ──────────────────────────────────────────────────────────────

ContainerManager::Status ContainerManager::status(const std::string& agent_id) {
    Status s;
    s.container = container_name(agent_id);
    s.image = kImage;
    s.workspace = ServerConfig::instance().workspace_root();
    if (!podman_available()) return s;

    auto r = run("podman inspect --format '{{.State.Status}}' " +
                 shell_quote(s.container));
    if (r.exit_code != 0) return s;  // no such container
    s.exists = true;
    s.running = r.output.find("running") != std::string::npos;
    return s;
}

std::string ContainerManager::ensure_running(const std::string& agent_id) {
    if (!podman_available())
        throw std::runtime_error("podman is not installed on this machine");

    ensure_image();

    // Company workspace: the configured HOST folder, bind-mounted into
    // every agent container at /workspace (Plan.md — company model).
    const std::string ws = ServerConfig::instance().workspace_root();

    const std::string cname = container_name(agent_id);
    auto st = status(agent_id);
    if (!st.exists) {
        fs::path home = agent_home_dir(agent_id);
        fs::create_directories(home);

        std::string cmd =
            std::string{"podman create --name "} + shell_quote(cname) +
            " --memory 4g --cpus 2"
            " -v " + shell_quote(ws) + ":/workspace"
            " -v " + shell_quote(home.string()) + ":/home/hive:U"
            " -w /workspace " + kImage;
        auto r = run(cmd);
        if (r.exit_code != 0)
            throw std::runtime_error("podman create failed: " + r.output);
    }

    st = status(agent_id);
    if (st.exists && !st.running) {
        auto r = run("podman start " + shell_quote(cname));
        if (r.exit_code != 0)
            throw std::runtime_error("podman start failed: " + r.output);
    }
    return cname;
}

bool ContainerManager::stop(const std::string& agent_id) {
    if (!podman_available()) return false;
    auto r = run("podman stop " + shell_quote(container_name(agent_id)),
                 /*timeout_s=*/30);
    return r.exit_code == 0;
}

bool ContainerManager::remove(const std::string& agent_id) {
    if (!podman_available()) return false;
    auto r = run("podman rm -f " + shell_quote(container_name(agent_id)),
                 /*timeout_s=*/30);
    return r.exit_code == 0;
}

hivemind::SecretStore* ContainerManager::secrets() {
    static hivemind::SecretStore store;   // XDG path; key loads on first use
    return &store;
}

ContainerManager::ExecResult ContainerManager::exec(const std::string& agent_id,
                                                    const std::string& command,
                                                    int timeout_s) {
    if (!podman_available())
        return {-1, "podman is not installed on this machine", false};

    // Phase 15: inject granted secrets via --env-file so values never show
    // up in podman's argv (visible in ps) or the shell history.
    std::string env_arg;
    int env_fd = -1;
    std::string env_path;
    auto env = secrets()->env_for_agent(agent_id);
    if (!env.empty()) {
        char tmpl[] = "/tmp/hive-env-XXXXXX";
        env_fd = mkstemp(tmpl);
        if (env_fd >= 0) {
            env_path = tmpl;
            ::chmod(env_path.c_str(), 0600);
            for (auto& [k, v] : env) {
                std::string line = k + "=" + v + "\n";
                (void)!write(env_fd, line.data(), line.size());
            }
            ::close(env_fd);
            env_fd = -1;
            env_arg = " --env-file " + shell_quote(env_path);
        }
    }
    std::string cmd =
        "podman exec -u hive -w /workspace" + env_arg + " " +
        shell_quote(container_name(agent_id)) +
        " bash -lc " + shell_quote(command);
    auto res = run(cmd, timeout_s);
    if (!env_path.empty()) {
        std::error_code ec;
        std::filesystem::remove(env_path, ec);   // gone the moment exec returns
    }
    return res;
}

// ── Phase 6: computer use (screen pipeline) ─────────────────────────────
// UNTESTED in the dev sandbox (no working podman) — verify on a real
// Ubuntu 26.04 box like the rest of the container layer.

void ContainerManager::ensure_browser_image() {
    if (!podman_available())
        throw std::runtime_error("podman is not installed on this machine");
    if (browser_image_present()) return;

    const char* containerfile = "hive-server/data/Containerfile.hive-agent-browser";
    if (!fs::exists(containerfile))
        throw std::runtime_error(
            std::string{"Containerfile not found at "} + containerfile +
            " (run hive-server from the repository root)");

    auto build = run(std::string{"podman build -t "} + kBrowserImage +
                         "  hive-server/data/Containerfile.hive-agent-browser"
                         " hive-server/data",
                     /*timeout_s=*/1800);
    if (build.exit_code != 0)
        throw std::runtime_error("browser image build failed: " + build.output);
}

bool ContainerManager::browser_image_present() {
    auto r = run(std::string{"podman image exists "} + kBrowserImage);
    return r.exit_code == 0;
}

uint16_t ContainerManager::screen_port(const std::string& agent_id) {
    const std::string cname = container_name(agent_id);
    auto r = run("podman port " + shell_quote(cname) + " 6080");
    if (r.exit_code != 0) return 0;
    // output looks like "127.0.0.1:32771" (possibly multiple lines)
    auto pos = r.output.rfind(':');
    if (pos == std::string::npos) return 0;
    try {
        return static_cast<uint16_t>(std::stoi(r.output.substr(pos + 1)));
    } catch (...) { return 0; }
}

uint16_t ContainerManager::ensure_screen(const std::string& agent_id) {
    ensure_browser_image();

    const std::string cname = container_name(agent_id);
    auto st = status(agent_id);

    // Containers created before Phase 6 have no published 6080 — recreate
    // (home dir is a bind mount, so browser profile/state survives).
    if (st.exists && screen_port(agent_id) == 0) {
        stop(agent_id);
        remove(agent_id);
        st = status(agent_id);
    }

    if (!st.exists) {
        fs::path home = agent_home_dir(agent_id);
        fs::create_directories(home);
        const std::string ws = ServerConfig::instance().workspace_root();
        std::string cmd =
            std::string{"podman create --name "} + shell_quote(cname) +
            " --memory 4g --cpus 2"
            " -p 127.0.0.1::6080"
            " -v " + shell_quote(ws) + ":/workspace"
            " -v " + shell_quote(home.string()) + ":/home/hive:U"
            " -w /workspace " + kBrowserImage;
        auto r = run(cmd);
        if (r.exit_code != 0)
            throw std::runtime_error("podman create (browser) failed: " + r.output);
    }

    st = status(agent_id);
    if (!st.running) {
        auto r = run("podman start " + shell_quote(cname));
        if (r.exit_code != 0)
            throw std::runtime_error("podman start failed: " + r.output);
    }

    const uint16_t port = screen_port(agent_id);
    if (port == 0)
        throw std::runtime_error("container has no published 6080 port");

    // Start the display stack idempotently (detached, logs to /tmp).
    auto r = exec(agent_id,
        "pgrep -x Xvfb >/dev/null || nohup Xvfb :99 -screen 0 1280x800x24 "
            ">/tmp/xvfb.log 2>&1 & "
        "sleep 1; "
        "pgrep -x x11vnc >/dev/null || nohup x11vnc -display :99 -forever "
            "-shared -nopw -localhost >/tmp/x11vnc.log 2>&1 & "
        "pgrep  websockify >/dev/null || nohup websockify --web "
            "/usr/share/novnc 6080 localhost:5900 >/tmp/websockify.log 2>&1 & "
        "sleep 2; pgrep  websockify >/dev/null",
        /*timeout_s=*/30);
    if (r.exit_code != 0)
        throw std::runtime_error("display stack failed to start: " + r.output);
    return port;
}

bool ContainerManager::stop_screen(const std::string& agent_id) {
    auto st = status(agent_id);
    if (!st.exists || !st.running) return false;
    exec(agent_id,
         "pkill  websockify; pkill -x x11vnc; pkill -x Xvfb; true",
         /*timeout_s=*/15);
    return true;
}
