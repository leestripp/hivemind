// runner.cpp — hive-worker slim podman executor (Phase 30.3).
//
// Deliberately NOT ContainerManager: the worker is a dumb executor with
// no SecretStore, no roster DB, no hive-brain. It replicates only the
// container shape (same image, same name scheme, per-agent workspace +
// home under ~/.local/share/hive-worker/) and runs argv verbatim.

#include "runner.h"

#include <cstdlib>
#include <sstream>
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
#include <pwd.h>
#include <cctype>
#include <cerrno>
#include <cstring>

#include <algorithm>
#include <chrono>
#include <climits>
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
| `.agents/skills/` | GLOBAL skill library shared by every agent on this worker. Writable — to create a shared skill, add `<skill-name>/SKILL.md` here; it is picked up live (no restart). The `/skills` mount is the read-only built-in library. |

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
        ".agents/skills",   // §Skills-Global: writable shared skill library
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

// ── Phase 72: host agents (bare-metal, sudo, NO sandbox) ─────────────
// A host agent runs the SAME hive-agent binary directly on this rig as a
// dedicated unix user (hivex-<name>) with passwordless sudo. There is no
// container. The two-level opt-in is: this worker's --allow-host-agents
// (g_allow_host_agents, advertised as the "host_agents" hello cap) AND the
// hub-side roster flag. See Plan.md "Host Agents (Phase 72)".

bool g_allow_host_agents = false;
std::string g_host_agent_binary;

std::string hive_agent_binary() {
    if (!g_host_agent_binary.empty()) return g_host_agent_binary;
    char buf[4096];
    const ssize_t n = ::readlink("/proc/self/exe", buf, sizeof buf - 1);
    if (n > 0) {
        buf[n] = '\0';
        const fs::path dir = fs::path(buf).parent_path();
        // Installed layout: hive-agent next to hive-worker; build tree:
        // <build>/hive-agent/hive-agent next to <build>/hive-worker/.
        for (const fs::path& p : {dir / "hive-agent",
                                  dir.parent_path() / "hive-agent" / "hive-agent"})
            if (fs::exists(p)) return p.lexically_normal().string();
    }
    return "/usr/local/bin/hive-agent";   // already-installed fallback
}

std::string current_user() {
    if (const passwd* pw = ::getpwuid(::geteuid())) return pw->pw_name;
    return {};
}

std::string host_user(const std::string& agent) {
    std::string u = "hivex-";
    for (char c : agent) {
        const char l = (char)std::tolower((unsigned char)c);
        if ((l >= 'a' && l <= 'z') || (l >= '0' && l <= '9') ||
            l == '_' || l == '-')
            u += l;
        else
            u += '-';
    }
    if (u.size() > 32) u.resize(32);   // Linux username cap
    return u;
}

std::string g_host_workspace_root;

std::string host_workspace(const std::string& agent) {
    // Shared, like the containers' /workspace bind-mount — every host
    // agent on the rig runs in the SAME directory.
    if (!g_host_workspace_root.empty()) return g_host_workspace_root;
    return "/home/" + host_user(agent);
}

bool host_agent_provisioned(const std::string& agent) {
    return ::getpwnam(host_user(agent).c_str()) != nullptr;
}

int run_as_user(const std::string& user,
                const std::vector<std::string>& args,
                const std::string& stdin_path, std::string* out) {
    int outfd[2];
    if (pipe(outfd) != 0) return -1;
    int infd = -1;
    if (!stdin_path.empty()) {
        infd = ::open(stdin_path.c_str(), O_RDONLY);
        if (infd < 0) { ::close(outfd[0]); ::close(outfd[1]); return -1; }
    }
    pid_t pid = fork();
    if (pid == 0) {
        if (infd >= 0) dup2(infd, STDIN_FILENO);
        dup2(outfd[1], STDOUT_FILENO);
        dup2(outfd[1], STDERR_FILENO);
        ::close(outfd[0]); ::close(outfd[1]);
        if (infd >= 0) ::close(infd);
        // -n: never prompt (NOPASSWD rule is provisioned); -H: target's
        // home as $HOME. argv VERBATIM, no shell — same discipline as the
        // podman exec path.
        std::vector<std::string> full{"-n", "-H", "-u", user};
        full.insert(full.end(), args.begin(), args.end());
        std::vector<char*> cargs;
        cargs.push_back(const_cast<char*>("sudo"));
        for (auto& a : full) cargs.push_back(const_cast<char*>(a.c_str()));
        cargs.push_back(nullptr);
        execvp("sudo", cargs.data());
        _exit(127);
    }
    ::close(outfd[1]);
    if (infd >= 0) ::close(infd);
    if (pid < 0) { ::close(outfd[0]); return -1; }
    char buf[512];
    ssize_t n;
    while ((n = ::read(outfd[0], buf, sizeof buf)) > 0)
        if (out) out->append(buf, (size_t)n);
    ::close(outfd[0]);
    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
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

// ── Phase 69: per-agent resource limits ──────────────────────────────
// The hub's spawn frame carries optional memory/cpus caps per agent
// (roster columns, PATCHable). fleet passes them to ensure_container,
// which PINS them to disk so the screen_ctl recreate path — which never
// sees a spawn frame — creates the container with the same limits.
// Defaults preserve the pre-Phase-68 shape: 4g / 2 cpus.
long long parse_memory_bytes(const std::string& s) {
    if (s.empty()) return -1;
    size_t i = 0;
    long long v = 0;
    while (i < s.size() && s[i] >= '0' && s[i] <= '9') {
        const long long d = s[i] - '0';
        if (v > (LLONG_MAX - d) / 10) return -1;   // overflow
        v = v * 10 + d;
        ++i;
    }
    if (i == 0) return -1;
    long long mult = 1;
    if (i < s.size()) {
        if (i + 1 != s.size()) return -1;
        switch (tolower((unsigned char)s[i])) {
            case 'b': mult = 1; break;
            case 'k': mult = 1024LL; break;
            case 'm': mult = 1024LL * 1024; break;
            case 'g': mult = 1024LL * 1024 * 1024; break;
            case 't': mult = 1024LL * 1024 * 1024 * 1024; break;
            default: return -1;
        }
    }
    if (v > LLONG_MAX / mult) return -1;
    return v * mult;
}

std::string format_cpus(double cpus) {
    if (cpus <= 0) return "0";
    char buf[32];
    snprintf(buf, sizeof buf, "%.4g", cpus);   // 12 -> "12", 1.5 -> "1.5"
    return buf;
}

struct AgentLimits {
    std::string memory;   // podman size string; "" = unset
    double cpus = 0;      // 0 = unset
};

fs::path limits_pin_path(const std::string& agent) {
    return fs::path(worker_dir()) / "limits" / agent;
}

AgentLimits read_limits_pin(const std::string& agent) {
    AgentLimits l;
    std::ifstream in(limits_pin_path(agent));
    if (!in) return l;
    std::getline(in, l.memory);
    std::string c;
    if (std::getline(in, c) && !c.empty()) {
        try { l.cpus = std::stod(c); } catch (...) { l.cpus = 0; }
    }
    return l;
}

void write_limits_pin(const std::string& agent, const AgentLimits& l) {
    const fs::path p = limits_pin_path(agent);
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    std::ofstream out(p, std::ios::trunc);
    out << l.memory << "\n" << format_cpus(l.cpus) << "\n";
}

// Explicit params win; missing pieces fall back to the pin, then the
// factory defaults. Exec-path ensure_container calls (no spec in hand)
// pass ""/0 and so never clobber a pin written by the fleet.
AgentLimits effective_limits(const std::string& agent,
                             const std::string& memory, double cpus) {
    AgentLimits l{memory, cpus};
    if (l.memory.empty() || l.cpus <= 0) {
        const AgentLimits pin = read_limits_pin(agent);
        if (l.memory.empty()) l.memory = pin.memory;
        if (l.cpus <= 0) l.cpus = pin.cpus;
    }
    if (l.memory.empty()) l.memory = "4g";
    if (l.cpus <= 0) l.cpus = 2;
    return l;
}

// cgroup limits are mutable on a LIVE container — reconcile drift with
// `podman update` instead of recreating (a recreate mid-turn kills the
// running brain). Failure is logged, never thrown: an older podman or
// cgroup-v1 host refusing the update must not wedge the spawn.
void reconcile_limits(const std::string& cname, const AgentLimits& l) {
    const long long want_mem = parse_memory_bytes(l.memory);
    if (want_mem <= 0) return;
    std::string out;
    if (podman_simple({"inspect", "-f",
                       "{{.HostConfig.Memory}} {{.HostConfig.NanoCpus}}",
                       cname}, &out) != 0)
        return;
    long long cur_mem = 0, cur_nano = 0;
    if (sscanf(out.c_str(), "%lld %lld", &cur_mem, &cur_nano) != 2) return;
    const long long want_nano = (long long)(l.cpus * 1e9 + 0.5);
    if (cur_mem == want_mem && cur_nano == want_nano) return;
    std::string err;
    if (podman_simple({"update", "--memory", l.memory,
                       "--cpus", format_cpus(l.cpus), cname}, &err) != 0) {
        std::cerr << "runner: podman update limits failed for " << cname
                  << ": " << err << "\n";
    } else {
        std::cerr << "runner: limits reconciled for " << cname
                  << " -> memory=" << l.memory
                  << " cpus=" << format_cpus(l.cpus) << "\n";
    }
}

// Serialize the container recreate paths: fleet ensure_container and
// screen_ctl start run on different threads and raced rm+create on the
// same container (2026-09-14: screen start 502'd as the fleet recreated
// the container onto :latest underneath it).
std::mutex& container_recreate_mutex() {
    static std::mutex m;
    return m;
}

// ── LAN service exposure (2026-10-01, Slate Ape / Bob) ─────────────
// $HIVE_LAN_PUBLISH: comma-separated extra podman -p specs published on
// ALL interfaces, e.g. "8090:8090" or "8090:8090,5173". Rootless pasta
// admits inbound ONLY via ports published on the agent's OWN container
// (a sidecar bridge cannot reach a pasta netns), so an agent serving a
// webapp to the LAN needs its port published here. Empty (default) =
// behavior unchanged.
static std::vector<std::string> lan_publish_args(const std::string& cname) {
    std::vector<std::string> out;
    // $HIVE_LAN_PUBLISH_AGENT: optional uuid gate for multi-agent rigs.
    // The publish spec alone is rig-wide, so a full-fleet recreate gave
    // EVERY fresh container the port — the first to start won and the
    // agent who actually serves (Bob/Slate Ape on ubuntu-pc) could
    // never start: pasta "Listen failed ... Address already in use"
    // (2026-10-07 OmniNet rollout). When set, only the container whose
    // name carries this uuid gets the extra publishes.
    const char* only = std::getenv("HIVE_LAN_PUBLISH_AGENT");
    if (only && *only && cname.find(only) == std::string::npos) return out;
    const char* env = std::getenv("HIVE_LAN_PUBLISH");
    if (!env || !*env) return out;
    std::istringstream ss(env);
    for (std::string spec; std::getline(ss, spec, ',');) {
        const auto b = spec.find_first_not_of(" \t");
        const auto e = spec.find_last_not_of(" \t");
        if (b == std::string::npos) continue;
        out.insert(out.end(), {"-p", spec.substr(b, e - b + 1)});
    }
    return out;
}

// True when the running container already binds every container-side
// port the env var asks for (PortBindings keys are "<port>/tcp"). Used
// to roll a newly-set env var out to pre-existing containers via the
// normal recreate path instead of a manual rm.
static bool lan_ports_present(const std::string& cname) {
    const auto specs = lan_publish_args(cname);
    if (specs.empty()) return true;   // nothing wanted — never triggers
    std::string json;
    if (podman_simple({"inspect", "-f",
                       "{{json .HostConfig.PortBindings}}", cname}, &json) != 0)
        return false;
    for (size_t i = 1; i < specs.size(); i += 2) {
        const std::string& spec = specs[i];
        const auto colon = spec.rfind(':');
        const std::string port =
            colon == std::string::npos ? spec : spec.substr(colon + 1);
        if (json.find("\"" + port + "/tcp\"") == std::string::npos)
            return false;
    }
    return true;
}

// ── Agent service-port publishing (2026-10-09, Bob/SlateApe ingress) ──
// Every agent container gets a FIXED block of host ports published 1:1
// onto its public-service range (container ports 8000+):
//   -p 0.0.0.0:<base>-<base+n-1>:8000-<8000+n-1>
// <base> is allocated once per agent and persisted in
// <worker_dir>/service_ports.txt, so http://<host>:<base+(port-8000)>
// survives recreation, image updates and worker restarts. Agents read
// their own mapping from the HIVEMIND_SERVICE_* env injected at create
// (the share_service tool derives the URL from it) — no tunnels, no
// allocation at use time, no recreate to expose a new service.
// Knobs (worker process env):
//   HIVE_SERVICE_POOL_BASE   first allocatable host port (default 18400)
//   HIVE_SERVICE_PORT_COUNT  ports per agent / range size (default 10;
//                            0 disables the feature)
//   HIVE_SERVICE_HOST        advertised address override (default: first
//                            non-loopback global IPv4)
// HIVE_LAN_PUBLISH (above) remains the manual escape hatch; keep its
// specs OUT of the pool range — the registry only tracks its own pool.
static int service_pool_base() {
    const char* env = std::getenv("HIVE_SERVICE_POOL_BASE");
    if (env && *env) {
        const int v = std::atoi(env);
        if (v > 0 && v + 100 <= 65536) return v;
    }
    return 18400;
}

int service_port_count() {
    const char* env = std::getenv("HIVE_SERVICE_PORT_COUNT");
    if (env && *env) {
        const int v = std::atoi(env);
        if (v >= 0 && v <= 100) return v;
    }
    return 10;
}

// First non-loopback, non-link-local global IPv4 — the address LAN
// clients dial. Overridable via HIVE_SERVICE_HOST (DNS name, tailnet
// IP, ...). Fallback loopback keeps the env well-formed on odd rigs.
std::string service_host_addr() {
    const char* env = std::getenv("HIVE_SERVICE_HOST");
    if (env && *env) return env;
    ifaddrs* ifs = nullptr;
    std::string found;
    if (getifaddrs(&ifs) == 0) {
        for (const ifaddrs* it = ifs; it && found.empty(); it = it->ifa_next) {
            if (!it->ifa_addr || it->ifa_addr->sa_family != AF_INET) continue;
            const auto* sin =
                reinterpret_cast<const sockaddr_in*>(it->ifa_addr);
            const unsigned long ip = ntohl(sin->sin_addr.s_addr);
            if ((ip >> 24) == 127 || (ip >> 16) == 0xA9FE) continue;
            char buf[INET_ADDRSTRLEN];
            if (inet_ntop(AF_INET, &sin->sin_addr, buf, sizeof buf))
                found = buf;
        }
        freeifaddrs(ifs);
    }
    return found.empty() ? "127.0.0.1" : found;
}

// Allocated base port for `agent`, read/allocate/persist via
// <worker_dir>/service_ports.txt ("<uuid> <base>" lines). Lowest free
// pool slot wins; an existing entry is stable forever.
int service_port_base(const std::string& agent) {
    static std::mutex reg_m;
    std::lock_guard<std::mutex> lk(reg_m);
    const int count = service_port_count();
    if (count <= 0) return 0;
    const fs::path reg = fs::path(worker_dir()) / "service_ports.txt";
    std::vector<std::pair<std::string, int>> rows;
    {
        std::ifstream in(reg);
        std::string id;
        int base = 0;
        while (in >> id >> base) rows.emplace_back(id, base);
    }
    for (const auto& [id, base] : rows)
        if (id == agent) return base;
    int base = service_pool_base();
    while (std::any_of(rows.begin(), rows.end(),
                       [&](const auto& r) { return r.second == base; }))
        base += count;
    rows.emplace_back(agent, base);
    const fs::path tmp = reg.string() + ".tmp";
    {
        std::ofstream out(tmp, std::ios::trunc);
        for (const auto& [id, b] : rows) out << id << ' ' << b << '\n';
    }
    std::error_code ec;
    fs::rename(tmp, reg, ec);
    if (ec) {   // cross-fs or odd fs: fall back to copy
        fs::copy_file(tmp, reg, fs::copy_options::overwrite_existing, ec);
        fs::remove(tmp, ec);
    }
    return base;
}

// podman -p spec publishing the agent's whole service range 1:1, or
// empty when the feature is disabled (HIVE_SERVICE_PORT_COUNT=0).
static std::vector<std::string> service_publish_args(const std::string& agent) {
    const int count = service_port_count();
    if (count <= 0) return {};
    const int base = service_port_base(agent);
    return {"-p", "0.0.0.0:" + std::to_string(base) + "-" +
                      std::to_string(base + count - 1) + ":" +
                      std::to_string(kServiceContainerBase) + "-" +
                      std::to_string(kServiceContainerBase + count - 1)};
}

// HIVEMIND_SERVICE_* env injected at create so the agent can derive its
// own service URLs without asking anyone (share_service reads these).
static std::vector<std::string> service_env_args(const std::string& agent) {
    const int count = service_port_count();
    if (count <= 0) return {};
    return {"-e", "HIVEMIND_SERVICE_HOST=" + service_host_addr(),
            "-e", "HIVEMIND_SERVICE_PORT_BASE=" +
                      std::to_string(service_port_base(agent)),
            "-e", "HIVEMIND_SERVICE_PORT_COUNT=" + std::to_string(count),
            "-e", "HIVEMIND_SERVICE_CONTAINER_BASE=" +
                      std::to_string(kServiceContainerBase)};
}

// True when the running container already publishes the container-side
// service range (PortBindings key "8000/tcp"). Gates a one-time rolling
// recreate after a worker upgrade adds the feature. Disabled feature =
// never triggers.
static bool service_range_present(const std::string& cname) {
    if (service_port_count() <= 0) return true;
    std::string json;
    if (podman_simple({"inspect", "-f",
                       "{{json .HostConfig.PortBindings}}", cname}, &json) != 0)
        return false;
    return json.find("\"" + std::to_string(kServiceContainerBase) + "/tcp\"")
           != std::string::npos;
}

// Agent boot hook (2026-10-09, Bob/SlateApe): after a (re)create, run
// /workspace/.hivemind/boot.sh detached when present+executable, so
// agent-owned services (web apps, tunnel replacements) come back without
// waiting for the agent's next turn. The hook is agent-OWNED — the
// worker only triggers it; output appends to .hivemind/boot.log.
void maybe_run_boot_hook(const std::string& agent) {
    std::string out;
    podman_simple({"exec", "-d", "-u", "hive", "-w", "/workspace",
                   container_name(agent), "sh", "-c",
                   "[ -x /workspace/.hivemind/boot.sh ] && "
                   "/workspace/.hivemind/boot.sh "
                   ">>/workspace/.hivemind/boot.log 2>&1 || true"},
                  &out);
}

void ensure_container(const std::string& agent, std::string image,
                      bool gpu, const std::string& memory, double cpus) {
    const std::string cname = container_name(agent);

    // Screen session pins the image (see screen_pinned_image).
    if (std::string pinned = screen_pinned_image(agent); !pinned.empty())
        image = pinned;

    // Phase 69: the fleet passes the hub's per-agent caps on every
    // spawn — pin them so spec-less paths (exec, screen recreate) reuse
    // the same limits. Spec-less callers pass ""/0 and keep the pin.
    if (!memory.empty() || cpus > 0)
        write_limits_pin(agent, {memory, cpus});
    const AgentLimits limits = effective_limits(agent, memory, cpus);

    ensure_image_built(image);

    std::lock_guard<std::mutex> recreate_lock(container_recreate_mutex());

    std::string state;
    if (podman_simple({"inspect", "--format", "{{.State.Status}}", cname},
                      &state) == 0) {
        if (state.find("running") != std::string::npos) {
            reconcile_limits(cname, limits);   // Phase 69: in-place, no recreate
            return;   // up
        }
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
            reconcile_limits(cname, limits);   // Phase 69
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
    // Phase 69: memory/cpus from the (pinned) per-agent limits;
    // --shm-size 1g matches the screen path — GUI apps and Chromium die
    // on podman's 64Mi default /dev/shm.
    std::vector<std::string> args{"create", "--name", cname,
                       "--memory", limits.memory,
                       "--cpus", format_cpus(limits.cpus),
                       "--shm-size", "1g", "--userns=keep-id",
                       "-p", "127.0.0.1::6080"};
    {
        const auto lan_args = lan_publish_args(cname);
        args.insert(args.end(), lan_args.begin(), lan_args.end());
    }
    {
        const auto svc_args = service_publish_args(agent);
        args.insert(args.end(), svc_args.begin(), svc_args.end());
        const auto svc_env = service_env_args(agent);
        args.insert(args.end(), svc_env.begin(), svc_env.end());
    }
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
    reconcile_limits(cname, limits);   // Phase 69: no-op on fresh create
    maybe_run_boot_hook(agent);
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
        if (screen_port_of(agent) == 0 || cur_img != new_img ||
            !lan_ports_present(cname) || !service_range_present(cname))
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
        // Phase 69: recreate with the agent's PINNED limits (the screen
        // path never sees a spawn frame — the pin written by the fleet's
        // ensure_container is the only source).
        const AgentLimits slimits = effective_limits(agent, "", 0);
        std::vector<std::string> cargs{"create", "--name", cname,
                           "--memory", slimits.memory,
                           "--cpus", format_cpus(slimits.cpus),
                           "--shm-size", "1g",
                           "--userns=keep-id",
                           "-p", "127.0.0.1::6080"};
        {
            const auto lan_args = lan_publish_args(cname);
            cargs.insert(cargs.end(), lan_args.begin(), lan_args.end());
        }
        {
            const auto svc_args = service_publish_args(agent);
            cargs.insert(cargs.end(), svc_args.begin(), svc_args.end());
            const auto svc_env = service_env_args(agent);
            cargs.insert(cargs.end(), svc_env.begin(), svc_env.end());
        }
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
        maybe_run_boot_hook(agent);
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

// ── Phase 72.2: bare-metal host-agent screens ─────────────────────────
// No container, no published port: hive-screen.sh runs ON this host as
// the hivex-<name> user (sudoers grant provisioned by hivex-provision)
// and websockify binds 6080+SLOT directly. The slot formula MUST match
// browser_driver.py::_default_slot and hive-screen.sh:_slot — the three
// implementations derive the same ports independently; change all or
// none. hivex-queenbee is pinned to slot 0 (her stack predates slots).
static int host_screen_slot(const std::string& user) {
    if (user == "hivex-queenbee") return 0;
    if (user.rfind("hivex-", 0) != 0) return 0;
    unsigned sum = 0;
    for (const unsigned char c : user) sum += c;
    return int(sum % 50) + 1;
}

ScreenResult cast_ctl(const std::string& /*agent_id*/,
                      const std::string& user,
                      const std::string& action) {
    ScreenResult r;
    r.kind = "cast";
    const int slot = host_screen_slot(user);
    const uint16_t port = static_cast<uint16_t>(16080 + slot);
    const int cdp = 9263 + slot;
    const std::string tag = "/tmp/hive-cast-" + user;

    if (action == "status") {
        if (port_accepts(port)) {
            r.port = port;
            r.running = true;
        }
        return r;
    }

    if (action == "stop") {
        std::string out;
        run_as_user(user, {"sh", "-c",
                           "kill $(cat " + tag + ".pid) 2>/dev/null; "
                           "rm -f " + tag + ".pid"},
                    "", &out);
        r.running = false;
        return r;
    }

    if (action != "start") { r.error = "unknown action"; return r; }

    if (port_accepts(port)) {          // already up — idempotent
        r.port = port;
        r.running = true;
        return r;
    }
    // Bridge script resolution (2026-10-21 reboot incident): the
    // canonical /usr/local/bin install needs root and was never done on
    // ubuntu-pc — every reboot stranded the screen until someone
    // relaunched by hand. Fall back to the worker user's repo checkout
    // (both rigs keep one at ~/hivemind) so the share link self-heals
    // with zero root steps. An outdated repo copy still beats no copy;
    // the next deploy refreshes it.
    const char* home = std::getenv("HOME");
    const std::string repo_script =
        (home ? std::string(home) : std::string()) +
        "/hivemind/hive-server/data/screen-bridge.py";
    const std::string script =
        fs::exists("/usr/local/bin/screen-bridge.py")
            ? "/usr/local/bin/screen-bridge.py"
            : (home && fs::exists(repo_script)) ? repo_script
            : "/usr/local/bin/screen-bridge.py";  // launch error surfaces
    std::string out;
    const std::string cmd =
        "nohup python3 " + script +
        " --serve-port " + std::to_string(port) +
        " --cdp-port " + std::to_string(cdp) +
        " >" + tag + ".log 2>&1 & echo $! >" + tag + ".pid";
    if (run_as_user(user, {"sh", "-c", cmd}, "", &out) != 0) {
        r.error = "cast bridge failed to launch: " + out;
        return r;
    }
    for (int i = 0; i < 50; ++i) {     // grace-probe up to ~10s
        if (port_accepts(port)) {
            r.port = port;
            r.running = true;
            return r;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    std::string log;
    run_as_user(user, {"sh", "-c", "tail -5 " + tag + ".log"}, "", &log);
    r.error = "cast bridge did not come up on " + std::to_string(port) +
              ": " + log;
    return r;
}

ScreenResult screen_ctl_host(const std::string& agent_id,
                             const std::string& user,
                             const std::string& action,
                             const std::string& resolution) {
    ScreenResult r;
    const uint16_t port =
        static_cast<uint16_t>(6080 + host_screen_slot(user));
    // Password cache: the script writes the 8-char RFB password into the
    // agent's unreadable home; it prints it on start ("PASSWORD=…") and
    // on the whitelisted "password" action, and we cache it here so
    // status polls stay cheap (the container path re-reads per status).
    const fs::path pwcache =
        fs::path(worker_dir()) / ("screen-" + agent_id + ".pw");
    auto read_cached_pw = [&]() -> std::string {
        std::ifstream in(pwcache);
        std::string pw;
        std::getline(in, pw);
        return pw;
    };
    auto fetch_pw = [&]() -> std::string {
        std::string pw = read_cached_pw();
        if (!pw.empty()) return pw;
        std::string out;
        if (run_as_user(user, {"/usr/local/bin/hive-screen.sh",
                               "password"}, "", &out) == 0) {
            while (!out.empty() && isspace((unsigned char)out.back()))
                out.pop_back();
            if (!out.empty()) {
                std::ofstream f(pwcache);
                f << out << "\n";
                pw = out;
            }
        }
        return pw;
    };

    if (action == "status") {
        // No published mapping to lie here — the listener IS websockify.
        if (port_accepts(port)) {
            r.port = port;
            r.running = true;
            r.password = fetch_pw();
        }
        return r;
    }

    if (action == "stop") {
        std::string out;
        run_as_user(user, {"/usr/local/bin/hive-screen.sh", "stop"},
                    "", &out);
        fs::remove(pwcache);
        r.running = false;
        return r;
    }

    if (action != "start") { r.error = "unknown action"; return r; }

    std::string out;
    if (run_as_user(user, {"/usr/local/bin/hive-screen.sh", resolution},
                    "", &out) != 0) {
        r.error = "host screen stack failed to start: " + out;
        return r;
    }
    // The script's last line on success is PASSWORD=<pw>.
    auto pos = out.rfind("PASSWORD=");
    if (pos != std::string::npos) {
        std::string pw = out.substr(pos + 9);
        while (!pw.empty() && isspace((unsigned char)pw.back()))
            pw.pop_back();
        if (!pw.empty()) {
            std::ofstream f(pwcache);
            f << pw << "\n";
        }
    }
    r.port = port;
    r.running = true;
    r.password = read_cached_pw();
    return r;
}

} // namespace hwrunner
