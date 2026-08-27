// runner.cpp — hive-worker slim podman executor (Phase 30.3).
//
// Deliberately NOT ContainerManager: the worker is a dumb executor with
// no SecretStore, no roster DB, no daedalus. It replicates only the
// container shape (same image, same name scheme, per-agent workspace +
// home under ~/.local/share/hive-worker/) and runs argv verbatim.

#include "runner.h"

#include <sys/types.h>
#include <sys/wait.h>
#include <poll.h>
#include <signal.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <stdexcept>
#include <iostream>

namespace fs = std::filesystem;

namespace {

constexpr const char* kImage = "localhost/hive-agent:latest";

std::string container_name(const std::string& agent) {
    // agent ids are server-generated ids; clamp to a safe charset anyway.
    std::string out = "hive-";
    for (char c : agent)
        out += (isalnum((unsigned char)c) || c == '-') ? c : '-';
    return out;
}

fs::path worker_dir() {
    return fs::path{std::getenv("HOME") ? std::getenv("HOME") : "."}
         / ".local" / "share" / "hive-worker";
}

/// Quick synchronous podman call (no streaming); returns exit code.
int podman_simple(const std::vector<std::string>& args, std::string* out) {
    std::string cmd = "podman";
    for (auto& a : args) {
        cmd += " '";
        for (char c : a) { if (c == '\'') cmd += "'\\''"; else cmd += c; }
        cmd += "'";
    }
    if (out) cmd += " 2>&1";
    FILE* p = popen(cmd.c_str(), "r");
    if (!p) return -1;
    if (out) {
        char buf[512];
        while (fgets(buf, sizeof buf, p)) *out += buf;
    }
    int rc = pclose(p);
    return WIFEXITED(rc) ? WEXITSTATUS(rc) : -1;
}

void ensure_container(const std::string& agent) {
    const std::string cname = container_name(agent);

    std::string img;
    if (podman_simple({"image", "exists", kImage}, nullptr) != 0) {
        // Same convention as the server: run from the repo root.
        if (!fs::exists("hive-server/data/Containerfile.hive-agent"))
            throw std::runtime_error(
                "image missing and Containerfile not found "
                "(run hive-worker from the repository root)");
        if (podman_simple({"build", "-t", kImage, "-f",
                           "--build-arg", "HIVE_UID=" + std::to_string(geteuid()),
                           "hive-server/data/Containerfile.hive-agent",
                           "hive-server/data"}, &img) != 0)
            throw std::runtime_error("podman build failed: " + img);
    }

    std::string state;
    if (podman_simple({"inspect", "--format", "{{.State.Status}}", cname},
                      &state) == 0) {
        if (state.find("running") != std::string::npos) return;   // up
        if (podman_simple({"start", cname}, &state) != 0)
            throw std::runtime_error("podman start failed: " + state);
        return;
    }

    // Per-agent workspace + home ON THE WORKER (no cross-machine share).
    fs::path base = worker_dir();
    fs::path ws = base / ("workspace-" + agent);
    fs::path home = base / ("home-" + agent);
    std::error_code ec;
    fs::create_directories(ws, ec);
    fs::create_directories(home, ec);

    std::string out;
    if (podman_simple({"create", "--name", cname, "--memory", "4g",
                       "--cpus", "2", "--userns=keep-id",
                       "-v", ws.string() + ":/workspace",
                       "-v", home.string() + "/:/home/hive:U",
                       "-w", "/workspace", kImage}, &out) != 0)
        throw std::runtime_error("podman create failed: " + out);
    if (podman_simple({"start", cname}, &out) != 0)
        throw std::runtime_error("podman start failed: " + out);
}

} // namespace

namespace hwrunner {

ExecOutcome run_container_exec(
        const std::string& agent, const std::vector<std::string>& argv,
        const std::string& env_content, int timeout_s,
        const std::function<void(std::string_view)>& on_chunk) {
    ExecOutcome oc;
    if (argv.empty()) { oc.error = "empty argv"; return oc; }
    const auto t0 = std::chrono::steady_clock::now();

    std::cerr << "runner: ensure_container " << agent << "\n";
    try { ensure_container(agent); }
    catch (const std::exception& e) { oc.error = e.what(); return oc; }
    std::cerr << "runner: container up, spawning podman exec\n";

    // Rendered env content → 0600 tmpfs file (deleted after the exec).
    std::string env_path;
    if (!env_content.empty()) {
        char tmpl[] = "/tmp/hivew-env-XXXXXX";
        int fd = mkstemp(tmpl);
        if (fd >= 0) {
            env_path = tmpl;
            ::chmod(env_path.c_str(), 0600);
            (void)!write(fd, env_content.data(), env_content.size());
            ::close(fd);
        }
    }

    // Build the podman argv: podman exec -u hive -w /workspace [--env-file F]
    // hive-<agent> <argv...>  — execvp, NO shell anywhere.
    std::vector<std::string> args{"exec", "-u", "hive", "-w", "/workspace"};
    if (!env_path.empty()) { args.push_back("--env-file"); args.push_back(env_path); }
    args.push_back(container_name(agent));
    args.insert(args.end(), argv.begin(), argv.end());

    int pipefd[2];
    if (pipe(pipefd) != 0) { oc.error = "pipe failed"; return oc; }

    pid_t pid = fork();
    if (pid == 0) {
        // child: stdout+stderr → pipe, then exec podman
        dup2(pipefd[1], STDOUT_FILENO);
        dup2(pipefd[1], STDERR_FILENO);
        ::close(pipefd[0]); ::close(pipefd[1]);
        std::vector<char*> cargs;
        cargs.push_back(const_cast<char*>("podman"));
        for (auto& a : args) cargs.push_back(const_cast<char*>(a.c_str()));
        cargs.push_back(nullptr);
        execvp("podman", cargs.data());
        _exit(127);   // exec failed
    }
    ::close(pipefd[1]);
    if (pid < 0) { ::close(pipefd[0]); oc.error = "fork failed"; return oc; }

    // Parent: stream the pipe until EOF / timeout.
    fcntl(pipefd[0], F_SETFL, O_NONBLOCK);
    std::string pending;
    const auto deadline = t0 + std::chrono::seconds(timeout_s);
    bool child_done = false;
    int status = 0;
    while (true) {
        char buf[8192];
        ssize_t n = read(pipefd[0], buf, sizeof buf);
        if (n > 0) {
            pending.append(buf, (size_t)n);
            // Flush at line boundaries or when the buffer is chunky.
            if (pending.size() >= 4096 || pending.find('\n') != std::string::npos) {
                on_chunk(pending);
                pending.clear();
            }
        }
        if (waitpid(pid, &status, WNOHANG) == pid) { child_done = true; break; }
        if (std::chrono::steady_clock::now() > deadline) {
            oc.timed_out = true;
            kill(pid, SIGKILL);
            waitpid(pid, &status, 0);
            break;
        }
        struct pollfd pfd{pipefd[0], POLLIN, 0};
        poll(&pfd, 1, 100);   // 100ms tick
        if (n <= 0 && (pfd.revents & POLLHUP)) {
            if (waitpid(pid, &status, WNOHANG) == pid) { child_done = true; break; }
        }
    }
    // drain any tail
    ssize_t n;
    char buf[8192];
    while ((n = read(pipefd[0], buf, sizeof buf)) > 0)
        pending.append(buf, (size_t)n);
    ::close(pipefd[0]);
    if (!pending.empty()) on_chunk(pending);
    if (oc.timed_out) on_chunk("\n[timed out]\n");

    if (child_done && WIFEXITED(status)) oc.exit_code = WEXITSTATUS(status);
    oc.duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0).count();

    if (!env_path.empty()) {
        std::error_code ec;
        fs::remove(env_path, ec);   // gone the moment the exec returns
    }
    return oc;
}

uint16_t screen_port_of(const std::string& agent) {
    std::string out;
    if (podman_simple({"port", container_name(agent), "6080"}, &out) != 0)
        return 0;
    auto pos = out.rfind(':');
    if (pos == std::string::npos) return 0;
    try { return static_cast<uint16_t>(std::stoi(out.substr(pos + 1))); }
    catch (...) { return 0; }
}

std::string screen_password_of(const std::string& agent) {
    std::string pw;
    if (podman_simple({"exec", container_name(agent),
                       "cat", "/home/hive/.vnc-password"}, &pw) != 0)
        return "";
    while (!pw.empty() && isspace((unsigned char)pw.back())) pw.pop_back();
    return pw;
}

ScreenResult screen_ctl(const std::string& agent, const std::string& action,
                        const std::string& image, const std::string& resolution) {
    ScreenResult r;
    const std::string cname = container_name(agent);

    if (action == "status") {
        r.port = screen_port_of(agent);
        r.running = r.port != 0;
        if (r.running) r.password = screen_password_of(agent);
        return r;
    }

    if (action == "stop") {
        std::string out;
        podman_simple({"exec", cname, "bash", "-lc",
                       "pkill websockify; pkill -x x11vnc; pkill -x Xvfb; true"},
                      &out);
        r.running = false;
        return r;
    }

    if (action != "start") { r.error = "unknown action"; return r; }

    // ── start ──────────────────────────────────────────────────────────
    std::string out;
    if (podman_simple({"image", "exists", image}, nullptr) != 0) {
        if (!fs::exists("hive-server/data/Containerfile.hive-agent-browser")) {
            r.error = "browser image missing and Containerfile not found "
                      "(run hive-worker from the repository root)";
            return r;
        }
        if (podman_simple({"build", "-t", image, "-f",
                           "--build-arg", "HIVE_UID=" + std::to_string(geteuid()),
                           "hive-server/data/Containerfile.hive-agent-browser",
                           "hive-server/data"}, &out) != 0) {
            r.error = "podman build (browser) failed: " + out;
            return r;
        }
    }

    // Recreate when the container is missing, has no published 6080, or
    // runs a stale/different image. Home + workspace are bind mounts —
    // they survive recreation.
    std::string state;
    const bool exists =
        podman_simple({"inspect", "--format", "{{.State.Status}}", cname},
                      &state) == 0;
    bool recreate = !exists;
    if (exists) {
        std::string cur_img, new_img;
        podman_simple({"inspect", "-f", "{{.Image}}", cname}, &cur_img);
        podman_simple({"image", "inspect", "-f", "{{.Id}}", image}, &new_img);
        if (screen_port_of(agent) == 0 || cur_img != new_img)
            recreate = true;
    }
    if (recreate) {
        podman_simple({"rm", "-f", cname}, &out);
        fs::path base = worker_dir();
        fs::path ws = base / ("workspace-" + agent);
        fs::path home = base / ("home-" + agent);
        std::error_code ec;
        fs::create_directories(ws, ec);
        fs::create_directories(home, ec);
        if (podman_simple({"create", "--name", cname, "--memory", "4g",
                           "--cpus", "2", "--shm-size", "1g",
                           "-p", "127.0.0.1::6080",
                           "-v", ws.string() + ":/workspace",
                           "-v", home.string() + "/:/home/hive:U",
                           "-w", "/workspace", image}, &out) != 0) {
            r.error = "podman create (browser) failed: " + out;
            return r;
        }
    }
    if (podman_simple({"inspect", "--format", "{{.State.Status}}", cname},
                      &state) != 0 ||
        state.find("running") == std::string::npos) {
        if (podman_simple({"start", cname}, &out) != 0) {
            r.error = "podman start failed: " + out;
            return r;
        }
    }

    r.port = screen_port_of(agent);
    if (r.port == 0) {
        r.error = "container has no published 6080 port";
        return r;
    }

    // Idempotent display-stack bring-up inside the container.
    if (podman_simple({"exec", cname, "hive-screen.sh", resolution},
                      &out) != 0) {
        r.error = "display stack failed to start: " + out;
        return r;
    }
    r.running = true;
    r.password = screen_password_of(agent);
    return r;
}

} // namespace hwrunner
