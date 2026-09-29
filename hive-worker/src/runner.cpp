// runner.cpp — hive-worker slim podman executor (Phase 30.3).
//
// Deliberately NOT ContainerManager: the worker is a dumb executor with
// no SecretStore, no roster DB, no hive-brain. It replicates only the
// container shape (same image, same name scheme, per-agent workspace +
// home under ~/.local/share/hive-worker/) and runs argv verbatim.

#include "runner.h"

#include <sys/types.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <ifaddrs.h>
#include <poll.h>
#include <signal.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>

#include <chrono>
#include <cstdio>
#include <fstream>
#include <mutex>
#include <thread>
#include <cstring>
#include <filesystem>
#include <stdexcept>
#include <iostream>

namespace fs = std::filesystem;

namespace hwrunner {

std::string container_name(const std::string& agent) {
    // agent ids are server-generated ids; clamp to a safe charset anyway.
    std::string out = "hive-";
    for (char c : agent)
        out += (isalnum((unsigned char)c) || c == '-') ? c : '-';
    return out;
}

std::string worker_dir() {
    return (fs::path{std::getenv("HOME") ? std::getenv("HOME") : "."}
         / ".local" / "share" / "hive-worker").string();
}

// Default /workspace map, written on first run only. Keep in sync with
// the hub-side skeleton (hive-server/data/workspace/index.md) — the key
// fact both must state: workspaces are PER-WORKER, never synced.
static const char* kDefaultIndex = R"(# /workspace — the company shared drive (THIS worker)

Every agent container **on this worker** mounts this same folder at
`/workspace` (read-write). It is the company drive: agents collaborate
through files here, not memory. This file is the map — read it before
doing file work, and keep it current as folders gain purpose.

## Workspaces are per-worker — there is NO sync

Each worker machine (rig) has its OWN independent `/workspace`. Files
you write here are INVISIBLE to agents homed on other workers, and
their files are invisible to you. There is no replication, no shared
mount, no background sync — by design (rigs can hold huge datasets).

Cross-rig hand-offs go through the THREAD, not the filesystem: say
where the work lives and what it's called, attach small files to the
thread, or ask the user to copy large ones. Never tell a teammate on
another rig to "just read it from /workspace".

## Layout

| Path | What lives there |
|---|---|
| `departments/<dept>/` | Department-owned work. Work in YOUR department's folder unless the task is explicitly cross-department. |
| `projects/<name>/` | Cross-department initiatives. One folder per project, with its own `README.md` (goal, status, who contributes). |
| `inbox/` | The user drops requests/files here for agents to pick up. Check it when asked "what's in the inbox". |
| `attachments/<thread-id>/` | Chat uploads, managed by the server. Read them; never reorganize. |
| `scratch/` | Temporary working files. May be cleaned at any time — nothing here is kept. |
| `archive/` | Finished/inactive work. Move, don't delete. |
| `.hivemind/` | Server-owned (prompts, internal state) — agents don't touch. |

## Departments

- `departments/chief-of-staff/` — coordination, cross-dept tracking, ops
- `departments/design/` — visual design, UI, wireframes, brand
- `departments/engineering/` — software development, code, infra
- `departments/executive/` — CEO: decisions, strategy docs, board materials
- `departments/marketing/` — content, campaigns, assets
- `departments/research/` — knowledge gathering, market intel, fast lookups

Each department folder gets a `README.md` naming its agents and local
conventions — create it when the department is created, keep it current.

## House rules

1. **Start in your department folder.** Cross-department work gets a
   `projects/<name>/` folder.
2. **Every project folder gets a README.md the day it's created**: goal,
   current status, where things stand. It's how the next agent (or the
   user) picks up the work without asking.
3. **Markdown and plain text first** — the universal interface; anyone
   (agent or human) can read them with no tools.
4. **Hand-offs are files, not memory** — and cross-rig hand-offs are
   MESSAGES, not files (see "per-worker" above).
5. **Names: kebab-case, ISO dates** (`2026-08-24-launch-plan.md`).
6. **Never delete** outside `scratch/`. Move to `archive/` instead.
7. `/home/hive` is your private container home — tools, notes, drafts
   not ready to share. The moment it's for the team, it goes to
   `/workspace`.
)";

void seed_workspace(const std::string& workspace_dir) {
    const fs::path ws{workspace_dir};
    std::error_code ec;
    static const char* kDirs[] = {
        "departments/chief-of-staff", "departments/design",
        "departments/engineering", "departments/executive",
        "departments/marketing", "departments/research",
        "projects", "inbox", "attachments", "scratch", "archive",
    };
    for (const char* d : kDirs)
        fs::create_directories(ws / d, ec);
    // Never overwrite: agents maintain the live map after first seed.
    if (fs::exists(ws / "index.md", ec)) return;
    std::ofstream out(ws / "index.md", std::ios::trunc);
    if (out) out << kDefaultIndex;
}

// Pasta hairpin fix (2026-09-21): under pasta the guest SHARES the host's
// public IP, so on the droplet itself leestrippai.com resolves IN-GUEST to
// the listenerless guest address and nginx :443/:8443 is unreachable. When
// THIS host is the apex, ensure_container maps the name to the podman
// host-gateway instead. Detected once: does the public name match one of
// our own non-loopback interfaces?
static bool self_hosted_apex() {
    static const bool cached = [] {
        addrinfo hints{};
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        addrinfo* res = nullptr;
        if (getaddrinfo("leestrippai.com", nullptr, &hints, &res) != 0 || !res)
            return false;
        bool match = false;
        for (auto* a = res; a && !match; a = a->ai_next) {
            const in_addr pub =
                reinterpret_cast<sockaddr_in*>(a->ai_addr)->sin_addr;
            ifaddrs* ifs = nullptr;
            if (getifaddrs(&ifs) != 0) break;
            for (auto* i = ifs; i && !match; i = i->ifa_next) {
                if (!i->ifa_addr || i->ifa_addr->sa_family != AF_INET) continue;
                const in_addr mine =
                    reinterpret_cast<sockaddr_in*>(i->ifa_addr)->sin_addr;
                if ((ntohl(mine.s_addr) >> 24) == 127) continue;  // loopback
                if (mine.s_addr == pub.s_addr) match = true;
            }
            freeifaddrs(ifs);
        }
        freeaddrinfo(res);
        return match;
    }();
    return cached;
}

/// Quick synchronous podman call (no streaming); returns exit code.
int podman_simple(const std::vector<std::string>& args, std::string* out) {
    std::string cmd = "podman";
    for (auto& a : args) {
        cmd += " '";
        for (char c : a) { if (c == '\'') cmd += "'\\''"; else cmd += c; }
        cmd += "'";
    }
    cmd += " 2>&1";
    FILE* p = popen(cmd.c_str(), "r");
    if (!p) return -1;
    // ALWAYS drain the pipe, even when the caller discards output: an
    // undrained popen pipe SIGPIPEs the child (rc 141) once it writes —
    // this silently broke the Phase 36 screen idle watcher's pgrep probe.
    char buf[512];
    while (fgets(buf, sizeof buf, p)) { if (out) *out += buf; }
    int rc = pclose(p);
    return WIFEXITED(rc) ? WEXITSTATUS(rc) : -1;
}

// Phase 35: build any missing image in the chain. ALL Containerfiles
// build from the repo ROOT: every COPY source is root-prefixed
// (build/hive-agent/hive-agent, hive-server/data/hive-screen.sh, ...).
// (An earlier revision built :browser from hive-server/data — that broke
// the moment the COPY lines gained their hive-server/data/ prefixes;
// found 2026-09-28 in a docs-vs-source audit. The path never fired in
// production because deploys pre-build the chain.)
// :gpu FROMs :browser FROMs :latest — recurse so a bare rig self-heals.
static void ensure_image_built(const std::string& image) {
    if (podman_simple({"image", "exists", image}, nullptr) == 0) return;
    if (!fs::exists("hive-server/data/Containerfile.hive-agent"))
        throw std::runtime_error(
            "image missing and Containerfile not found "
            "(run hive-worker from the repository root)");

    const bool is_browser = image == "localhost/hive-agent:browser";
    const bool is_gpu     = image == "localhost/hive-agent:gpu";
    if (is_gpu)     ensure_image_built("localhost/hive-agent:latest");  // Phase 68d: two-base tree
    if (is_browser) ensure_image_built("localhost/hive-agent:latest");

    const std::string cf  = is_gpu ? "hive-server/data/Containerfile.hive-agent-gpu"
                        : is_browser ? "hive-server/data/Containerfile.hive-agent-browser"
                                     : "hive-server/data/Containerfile.hive-agent";
    const std::string ctx = ".";   // repo root — see header comment
    std::string img;
    if (podman_simple({"build", "-t", image,
                       "--build-arg", "HIVE_UID=" + std::to_string(geteuid()),
                       "-f", cf, ctx}, &img) != 0)
        throw std::runtime_error("podman build failed: " + img);
}

// ── Phase 54: screen-image pinning ─────────────────────────────────────
// A screen session pins the container to the desktop image (:browser /
// :gpu). screen_ctl start writes this marker, stop removes it;
// ensure_container honours it so the fleet drift-check doesn't "correct"
// a screen container back onto :latest mid-session — and so a stopped
// screen container comes BACK on the screen image after a worker
// restart (marker persists on disk).
fs::path screen_marker_path(const std::string& agent) {
    return fs::path(worker_dir()) / ("screen-" + agent);
}

std::string screen_pinned_image(const std::string& agent) {
    std::ifstream in(screen_marker_path(agent));
    std::string tag;
    std::getline(in, tag);
    while (!tag.empty() && isspace((unsigned char)tag.back())) tag.pop_back();
    return tag;
}

// Serialize the container recreate paths: fleet ensure_container and
// screen_ctl start run on different threads and raced rm+create on the
// same container (2026-09-14: screen start 502'd as the fleet recreated
// the container onto :latest underneath it).
std::mutex& container_recreate_mutex() {
    static std::mutex m;
    return m;
}

void ensure_container(const std::string& agent, std::string image,
                      bool gpu) {
    const std::string cname = container_name(agent);

    // Screen session pins the image (see screen_pinned_image).
    if (std::string pinned = screen_pinned_image(agent); !pinned.empty())
        image = pinned;

    ensure_image_built(image);

    std::lock_guard<std::mutex> recreate_lock(container_recreate_mutex());

    std::string state;
    if (podman_simple({"inspect", "--format", "{{.State.Status}}", cname},
                      &state) == 0) {
        if (state.find("running") != std::string::npos) return;   // up
        // Stopped container: recreate when the image DRIFTED (dep bumps,
        // screen-stack changes) so a restart actually picks up the new
        // image — plain `podman start` would resurrect the old one
        // forever (hit 2026-09-03: hive-brain 1.0.3 images rebuilt, agents
        // still on the old IDs). Running containers are left alone —
        // they adopt the new image at their next natural restart.
        auto trimmed = [](std::string v) {
            while (!v.empty() && (v.back() == '\n' || v.back() == ' ' ||
                                  v.back() == '\t' || v.back() == '\r'))
                v.pop_back();
            return v;
        };
        std::string have, want;
        podman_simple({"inspect", "--format", "{{.Image}}", cname}, &have);
        podman_simple({"image", "inspect", "--format", "{{.Id}}", image},
                      &want);
        if (!trimmed(have).empty() && !trimmed(want).empty() &&
            trimmed(have) != trimmed(want)) {
            std::cerr << "runner: image drifted for " << cname
                      << " — recreating on " << image << "\n";
            podman_simple({"rm", "-f", cname}, nullptr);
            // fall through to the create path below
        } else {
            if (podman_simple({"start", cname}, &state) != 0)
                throw std::runtime_error("podman start failed: " + state);
            return;
        }
    }

    // Shared workspace ON THIS WORKER (2026-08-29): all agents on the rig
    // see the same /workspace, mirroring the server's company-drive model.
    // Per-agent remains only the home dir. (Was workspace-<agent> — agents
    // on the same worker couldn't collaborate.)
    fs::path base = worker_dir();
    fs::path ws = base / "workspace";
    fs::path home = base / ("home-" + agent);
    fs::path skills = base / "skills";   // Phase 33: synced skill library
    std::error_code ec;
    fs::create_directories(ws, ec);
    fs::create_directories(home, ec);
    fs::create_directories(skills, ec);

    // Phase 36: ALWAYS publish loopback 6080 (dynamic host port) — the
    // screen stack then starts by pure exec, with NO container recreate
    // (recreating to add the port killed the running agent mid-turn).
    std::vector<std::string> args{"create", "--name", cname, "--memory", "4g",
                       "--cpus", "2", "--userns=keep-id",
                       "-p", "127.0.0.1::6080"};
    if (gpu) {
        // Phase 35: AMD GPU passthrough. /dev/kfd = HIP compute (REQUIRED —
        // without it rocminfo sees nothing), /dev/dri = GL/Vulkan render
        // nodes. keep-groups carries the host `render` group membership in.
        args.insert(args.end(), {"--device", "/dev/kfd",
                                 "--device", "/dev/dri",
                                 "--group-add", "keep-groups"});
    }
    // Pasta hairpin fix (2026-09-21, see self_hosted_apex): on the droplet
    // the apex self-resolves to the listenerless guest; host-gateway makes
    // nginx :443/:8443 reachable in-container. Harmless elsewhere.
    if (self_hosted_apex())
        args.insert(args.end(), {"--add-host", "leestrippai.com:host-gateway"});
    args.insert(args.end(), {"-v", ws.string() + ":/workspace",
                       "-v", home.string() + "/:/home/hive:U",
                       "-v", skills.string() + ":/skills:ro",
                       "-w", "/workspace", image});
    std::string out;
    if (podman_simple(args, &out) != 0)
        throw std::runtime_error("podman create failed: " + out);
    if (podman_simple({"start", cname}, &out) != 0)
        throw std::runtime_error("podman start failed: " + out);
}

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

/// True when a loopback TCP connect to the published host port succeeds —
/// the only honest "can the splice reach it" test. Rootless port
/// forwarding (pasta) is occasionally missing after create+start; the
/// mapping shows in `podman port` but refuses connections until the
/// container is restarted (gpu-rig, podman 5.7.0, 2026-09-14).
bool port_accepts(uint16_t port) {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return false;
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    const bool ok = ::connect(fd, reinterpret_cast<sockaddr*>(&addr),
                              sizeof(addr)) == 0;
    ::close(fd);
    return ok;
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
                        const std::string& type, const std::string& resolution) {
    ScreenResult r;
    const std::string cname = container_name(agent);
    const bool gpu = (type == "gpu");
    // Screen needs the desktop stack: standard agents get recreated from
    // :browser; gpu agents from :gpu (which is FROM :browser). Recreating
    // a gpu container from :browser would silently drop the GPU devices —
    // that bug clobbered Forge's container twice (2026-09-02).
    const std::string image =
        gpu ? "localhost/hive-agent:gpu" : "localhost/hive-agent:browser";

    if (action == "status") {
        r.port = screen_port_of(agent);
        // The published port mapping outlives the screen stack: the idle
        // watcher tears websockify/x11vnc down but the 6080 mapping
        // persists, so the port alone LIES about "running" (hub then
        // redirects to a dead splice with an empty password). websockify
        // owns the listener — no websockify, no screen.
        if (r.port != 0 &&
            (podman_simple({"exec", cname, "pgrep", "-f", "websockify"},
                           nullptr) != 0 ||
             !port_accepts(r.port)))
            r.port = 0;
        r.running = r.port != 0;
        if (r.running) r.password = screen_password_of(agent);
        return r;
    }

    if (action == "stop") {
        // Phase 36: the script knows its own watchdog/pidfile — pkill
        // trios here let the watchdog revive x11vnc (stop never stuck).
        std::string out;
        podman_simple({"exec", cname, "hive-screen.sh", "stop"}, &out);
        fs::remove(screen_marker_path(agent));   // Phase 54: unpin
        r.running = false;
        return r;
    }

    if (action != "start") { r.error = "unknown action"; return r; }

    // ── start ──────────────────────────────────────────────────────────
    std::string out;
    if (podman_simple({"image", "exists", image}, nullptr) != 0) {
        // gpu Containerfile builds from repo root; browser from data/.
        const char* cf = gpu ? "hive-server/data/Containerfile.hive-agent-gpu"
                             : "hive-server/data/Containerfile.hive-agent-browser";
        const char* ctx = gpu ? "." : "hive-server/data";
        if (!fs::exists(cf)) {
            r.error = std::string("image missing and ") + cf + " not found "
                      "(run hive-worker from the repository root)";
            return r;
        }
        if (podman_simple({"build", "-t", image,
                           "--build-arg", "HIVE_UID=" + std::to_string(geteuid()),
                           "-f", cf, ctx}, &out) != 0) {
            r.error = "podman build (screen image) failed: " + out;
            return r;
        }
    }

    // Phase 54: pin the screen image BEFORE touching the container so a
    // concurrent fleet reconcile targets the same image instead of
    // "correcting" the container back onto :latest mid-bring-up.
    {
        std::ofstream pin(screen_marker_path(agent));
        pin << image << "\n";
    }
    std::lock_guard<std::mutex> recreate_lock(container_recreate_mutex());

    // Phase 36: ensure_container always publishes 6080 now, so recreate
    // is only a LEGACY fallback (pre-Phase-36 containers without the
    // port) or for a stale/different image. Home + workspace are bind
    // mounts — they survive recreation.
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
        fs::path ws = base / "workspace";   // shared per-worker (see ensure_container)
        fs::path home = base / ("home-" + agent);
        fs::path skills = base / "skills";  // Phase 33
        std::error_code ec;
        fs::create_directories(ws, ec);
        fs::create_directories(home, ec);
        // MUST match ensure_container's userns: without keep-id the
        // recreated container maps hive->subuid, every keep-id-written
        // file appears root-owned, and /workspace goes read-only for the
        // agent (2026-09-16 flip-flop incident — "perms fix not holding").
        std::vector<std::string> cargs{"create", "--name", cname,
                           "--memory", "4g", "--cpus", "2", "--shm-size", "1g",
                           "--userns=keep-id",
                           "-p", "127.0.0.1::6080"};
        if (gpu)   // same passthrough contract as ensure_container
            cargs.insert(cargs.end(), {"--device", "/dev/kfd",
                                       "--device", "/dev/dri",
                                       "--group-add", "keep-groups"});
        // Pasta hairpin fix — MUST match ensure_container: a screen-path
        // recreate without this silently drops the apex hosts entry and
        // searxng/hub become unreachable in-container (2026-09-27 PA
        // searxng outage: container recreated by this path, entry lost).
        if (self_hosted_apex())
            cargs.insert(cargs.end(),
                         {"--add-host", "leestrippai.com:host-gateway"});
        cargs.insert(cargs.end(), {
                           "-v", ws.string() + ":/workspace",
                           "-v", home.string() + "/:/home/hive:U",
                           "-v", skills.string() + ":/skills:ro",
                           "-w", "/workspace", image});
        if (podman_simple(cargs, &out) != 0) {
            r.error = "podman create (screen image) failed: " + out;
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
    // Rootless forwarding (pasta) can lag or be missing right after
    // create+start (see port_accepts) — grace-probe first, then one
    // container restart, then grace-probe again before giving up.
    // 45s: pasta took ~30-45s to listen after a container restart on
    // gpu-rig (09:11 502 with a 15s window; retry converged).
    auto accepts_soon = [&] {
        for (int i = 0; i < 45; ++i) {
            if (port_accepts(r.port)) return true;
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
        return false;
    };
    if (!accepts_soon()) {
        std::cerr << "runner: published 6080 refuses connections for "
                  << cname << " — restarting container to re-establish "
                     "port forwarding\n";
        podman_simple({"restart", cname}, &out);
        if (!accepts_soon()) {
            r.port = 0;
            r.error = "published 6080 port refuses connections";
            return r;
        }
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
