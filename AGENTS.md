# AGENTS.md — HiveMind 2.0 (0.5.3)

> Quick-start reference for AI coding agents. Read this before touching code.
> Deeper rationale: `docs/design.md`. Roadmap & phase status: `Plan.md`.

---

## 1. What this is

HiveMind 2.0 is a **self-hosted team of always-on AI agents for Linux**
(Ubuntu 26.04 target; no Windows/macOS). Persistent named teammates
(`daedalus::Agent`) you message in threads; they run commands in rootless
podman containers as user `hive` (passwordless sudo) — on the server by
default or on any enrolled `hive-worker` machine — delegate to each other,
deliver files to each other across machines (`send_file` → the server's
company workspace), wait for human approval when gated, and fire scheduled
routines. Agents on the same worker rig share ONE `/workspace` mount so
siblings can collaborate on the same project files. The 1.x
node-graph engine was **removed** (Phase 12); threads + fan-in are the
orchestration model.

| Property | Value |
|----------|-------|
| Language | **C++23** |
| Build | **CMake 3.28+** superbuild — `./hivemindctl build` |
| Server | `hive-server/` → `build/hive-server/hive-server` (OmniNet, :8421) |
| Client | `hive-client/` → `build/hive-client/hive-client` (GTK4/gtkmm4) |
| Worker | `hive-worker/` → `build/hive-worker/hive-worker` (OmniNet-only, dials out to :8423) |
| Agent engine | **daedalus** (FetchContent, tag `0.9.6` via `-DDAEDALUS_TAG`) |
| HTTP/WS | **OmniNet** (FetchContent, `main`) — auto-updated at configure time via `OMNINET_AUTO_UPDATE` (default ON); checkouts have a local-edits guard. The ONLY HTTP client/stack. |
| Tests | `ctest --test-dir build` or `./hivemindctl test` — 20 tests, built `-UNDEBUG` |

## 2. Hard rules (learned the expensive way)

1. **Never patch OmniNet or daedalus.** Consume public APIs as-is; bugs are
   in our code until proven otherwise. If OmniNet lacks a capability,
   extend OmniNet upstream — never reintroduce curl (no libcurl, no HTTP
   shell-outs anywhere; `OmniNet::HTTP::Client` only).
2. **LLMStore owns LLM config** (`~/.local/share/hivemind/llm.db`) — Phase
   13 superseded the old "no LLM settings" rule. Providers/models/creds
   are managed via /api/providers + /api/models (web Models page);
   AgentHost INJECTS them into daedalus AgentConfig explicit fields at
   spawn. daedalus' own SettingsStore is NOT consulted.
3. **Agents never execute on the host.** Containers are mandatory — the
   old `workspace_mode:"host"` escape hatch was REMOVED (2026-08-25);
   legacy rows normalize to `shared` on load. Commands go through
   `container_exec` → podman — locally, or over the worker channel when
   the agent's `worker` field names an enrolled worker.
4. **daedalus history persists ONLY via `Agent::shutdown()`** — `~Agent`
   does not save, and turn-loop mutations bypass auto-flush. Server signal
   path calls `AgentRegistry::shutdown_all()`. Never `kill -9` the server
   when you care about history (SIGINT stops it gracefully; SIGTERM's flag
   path has been seen to hang).
5. **Serialize all daedalus Agent lifecycles** with `agent_exec_mutex()`
   (recursive — nested delegation turns are same-thread). daedalus 0.9.6
   shares global state; parallel agents crash it.
6. **OmniNet `pathParam(i)` is unreliable for multi-param routes** — parse
   `req.path()` with a local `path_segments()` helper (see any *Routes.cpp).
7. **Register API routes BEFORE `register_static_routes`** (first-match-wins;
   the static catch-all is last).
8. **Disabled tool names hide host tools too** — a replacement for a
   built-in tool needs a DISTINCT name (that's why `container_exec` exists).
9. **Kill dev servers with `pgrep -x hive-server`**, never a `-f` pattern
   that matches your own shell's command line.
10. Run server AND worker from the **repo root** (web/dist, Containerfile
    paths are cwd-relative — and `systemd-run` does NOT preserve your cd;
    use `--working-directory=` or `bash -c 'cd … && exec …'`).
11. **Ownership rule (no refcount band-aids):** the AgentRegistry is the
    SOLE owner of AgentHosts (`unique_ptr`). Hosts are never destroyed
    mid-process — `evict()` marks dead, saves history (shutdown()), and
    retires to the graveyard; destruction happens once, in
    `shutdown_all()`, after `ChatService::quiesce()` has drained all turns.
    References from `get_or_create()` are valid for the process lifetime.
    Do NOT reintroduce shared_ptr here.
12. **Worker-channel WS gotchas (Phase 30):** `Config.keepAliveTimeout`
    must be **0** on the worker channel (it's applied as SO_RCVTIMEO and
    inherited by upgraded WS sockets — 5s of worker silence during
    `podman create` reads as peer-closed). And OmniNet
    `receive()/receiveText()` use per-call buffers — coalesced frames in
    one TCP segment are silently discarded; drive the socket with
    `readLoop()` + onText callbacks on BOTH ends.
13. **wss:// WebSockets died after ~2s of peer silence (fixed upstream
    2026-08-29).** OmniNet's TLSSocket::recvAll() returns an EMPTY buffer
    on its ~2s idle-select cap, and Connection::readLoop()/receive() read
    that as "closed" — every TLS WebSocket died at the first quiet moment
    (HiveMind's worker channel flapped every ~15s). Fixed in OmniNet
    6e3d036 via Socket::hasPeerClosed() (SSL_peek probe); ws:// was never
    affected. If wss connections drop "for no reason", check your vendored
    OmniNet has that commit.
14. **FetchContent `GIT_TAG main` goes stale silently.** `FetchContent`
    caches `_deps/<dep>-src` and does NOT re-fetch when the declared tag is
    unchanged. Our root CMake now runs `git fetch && reset --hard origin/main`
    at configure time for OmniNet (`OMNINET_AUTO_UPDATE`, default ON, skipped
    if dirty). This is how we caught the 1.0.0 connection-thread leak fix; without
    it you can vendor an old `main` for weeks.

15. **`CMAKE_INSTALL_PREFIX` must be a CACHE default** — a plain
    `set()` after `project()` silently resolved `$ENV{HOME}` at configure
    time and baked the wrong user's path into the .desktop `Exec=`
    (GNOME dropped the launcher). Use `INITIALIZED_TO_DEFAULT` + CACHE
    FORCE; `cmake --install --prefix` redirects file DESTINATIONS but NOT
    content baked by `configure_file` (like .desktop Exec paths).

## 3. Layout map

```
hive-server/src/            AgentHost (+Registry, ChatService), stores:
                            Agent/Thread/Approval/Routine/Auth/Secret/
                            LLM/Connector/ExecAudit/WorkerStore,
                            ContainerManager, WorkerChannel, WorkerRouter,
                            BrowserTools, Files, Schedule, Vault
hive-server/include/hivemind/routes/   REST registrars (h+cpp side by side)
hive-server/data/           Containerfile.hive-agent (+browser variant),
                            hive-screen.sh, hive-server.service
hive-client/src/            HiveApplication, HiveWindow, ApiClient
hive-worker/src/            main.cpp (WS client loop), runner (podman ops)
web/dist/                   zero-build web dashboard (pages/ per view)
tests/                      one binary per store/subsystem (20 tests)
```

State at runtime (`~/.local/share/hivemind/`): `agents.db`, `threads.db`,
`approvals.db`, `routines.db`, `auth.db`, `llm.db`, `secrets.db`,
`connectors.db`, `exec_audit.db`, `workers.db`, `skills/` (global library),
`agents/<id>/` (per-agent daedalus dir + container home), `workspace/`
(shared company volume). Worker rigs keep their state at
`~/.local/share/hive-worker/`: `workspace/` (ONE shared mount for all
containers on that rig, Phase 31), `home-<agentid>/` (per-agent homes).

## 4. Common tasks

```bash
./hivemindctl build && ./hivemindctl start   # build + run server :8421
./hivemindctl test                           # 20-test suite
./hivemindctl client                         # GTK client
./hivemindctl logs                           # follow server log
hive-server enroll-worker <name>             # mint hmwk_ key (shown once)
```

**Add a store:** copy `AgentStore` (own .db, fresh sqlite conn per op, path
ctor for tests). **Add routes:** `routes/FooRoutes.{h,cpp}` + register in
`main.cpp` before static routes. **Add an agent tool:** subclass
`daedalus::BaseTool`, register in `AgentHost::ensure_agent()` with a unique
name. **Add a test:** `tests/test_foo.cpp` + `hivemind_test(test_foo)`.
**Add a web page:** `web/dist/js/pages/foo.js` + page div + sidebar button
in `index.html` + wire `main.js` (copy `workers.js` — the newest).

## 5. Environment notes (this dev sandbox)

- podman is installed but **containers cannot run here** (nested userns) —
  `test_container_manager` skips its live section; run it on a real box.
  The worker binary builds locally but live-testing happens on dev-rig.
- No display: smoke-launch the client with `xvfb-run -a`.
- Live test model: **kimi-for-coding** (Kimi K2.7).
- Rig deployment: dev-rig runs hive-server + hive-worker as systemd USER
  units (`hivemind-dev`, `hive-worker-dev-rig`), CWD = repo root.

## 6. Agent runtime details (what every AgentHost gets)

These are implementation facts that change rarely but trip up new work:

### Tools registered in `AgentHost::ensure_agent()`

Containerized agents get `container_exec` plus the browser suite;
all roster agents get the collaboration/routine tools:

| Tool | Purpose |
|---|---|
| `container_exec` | Bash in the agent's podman container (`/workspace`, user `hive`, sudo). Host `execute_bash` is DISABLED; the replacement must be named something else (see rule 8). |
| `browser_navigate/click/type/press/read/screenshot/eval/tabs/back/reload` | Headless Chromium over CDP inside the container; visible via the web **Computer** tab. |
| `browser_fill_secret` | Fill a form from the agent's encrypted secret store; approval-gated when `require_approval` is on. |
| `list_agents` | Roster list (used for delegation). |
| `message_agent` | Async delegation: posts to the pair thread; fan-in wakes the asker. Do NOT make this synchronous. |
| `send_file` | Deliver a file from the agent's /workspace to the company workspace on the server (Phase 31). Confined dest (parent must exist), 50MB cap, `overwrite` opt-in, optional `notify` teammate. Remote agents: bytes ride the worker channel (`file_fetch` op). |
| `create_agent` | Add a teammate; optional `worker` field places the container on an enrolled `hive-worker`. |
| `list_workers` | Online/offline worker rigs. |
| `search_skills` / `install_skill` | Skill marketplace discovery: keyword-search the registry (`web/dist/skills-registry.json`), then git-install into the shared library + auto-enable for the caller. Agents should search here FIRST before web-searching for a capability. |
| `save_routine` / `list_routines` / `update_routine` / `delete_routine` | Own-routines-only; `id` or name accepted. |

### Routine schedules (UTC only)

Supported `schedule.type`: `once`, `interval`, `daily`, `weekly`. Time strings
are always `"HH:MM"` UTC; weekly uses `days: [0..6]` with 0=Sunday.
`Schedule::validate_schedule()` and `compute_next_fire()` are the source of
truth. The 30-second scheduler runs from `AgentHost`/main.cpp.

### Approvals & watchdog

- Approval requests wait **600 s** (`AgentHost::request_approval()` →
  `ApprovalStore::wait(id, 600)`); auto-review rules short-circuit this.
- A single agent turn is capped at **240 s** (`HIVEMIND_TURN_TIMEOUT_S`,
  default 240). A hung LLM call is aborted so the agent's queue unblocks.
- `require_approval: true` gates `container_exec` and browser actions.

### Thread/context model

- Threads fix participants at creation; there is no "add agent to existing
  thread" flow.
- Conversation history lives in daedalus' `messages.json`, restored on every
  AgentHost reconstruction. `ChatService::fan_in` sends ONLY the new message;
  do not prepend backlogs.
- Deleting an agent cascades: solo threads deleted, group threads keep going
  with a system note, routines deleted or reassigned via `?reassign_to=`.
  The on-disk agent dir is kept; the podman container is removed best-effort.

### Skills

- **Global skills** (`~/.local/share/hivemind/skills/`) are shared; enabling
  one symlinks it into the agent's `~/.agents/skills/`.
- **Local skills** are per-agent, learned per-agent, owned per-agent.
- Format is Anthropic SKILL.md. See `docs/skills.md` for the enable flow.

## 7. Current status (0.5.3, 2026-08-29)

Phases 0–31 done and user-verified (see Plan.md). Workers now connect over
**wss://** through nginx (30.5 TLS effectively live; the OmniNet
idle-close bug it exposed is fixed upstream, rule 13). Phase 31: shared
per-rig workspaces + the `send_file` tool (worker → company workspace,
confined, 50 MB cap). Also this cycle: container network self-heal (dead
pasta detection via /proc/net/route), client roster selection persistence
fix, app icon + .desktop launcher (install-prefix pitfall = rule 15).
Open: screen cold-start readiness race, packaging follow-ups (first real
Flatpak build).

**Auth:** all `/api/*` calls need `Authorization: Bearer <key>`; the master
key prints once at first start. Worker channel auth: `hmwk_` keys from
`enroll-worker` (SHA-256 hashed in workers.db).
