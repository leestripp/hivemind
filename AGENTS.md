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
2. **LLM config is WORKER-owned.** Each hive-worker keeps
   `~/.local/share/hive-worker/models.json` + `secrets.json` (0600) and
   injects credentials into agents as spawn env (`HIVEMIND_*`). The hub
   holds NO LLM keys (LLMStore was deleted); `GET /api/models` is the
   union of connected workers' hello catalogs. A fresh worker without
   models.json spawns agents that fail at initialize.
3. **Agents never execute on the host.** Containers are mandatory — the
   old `workspace_mode:"host"` escape hatch was REMOVED (2026-08-25);
   legacy rows normalize to `shared` on load. Commands go through
   `container_exec` → podman — locally, or over the worker channel when
   the agent's `worker` field names an enrolled worker.
4. **daedalus history persists ONLY via `Agent::shutdown()`.**
   In v2 that means hive-agent's `shutdown` op (sent by the worker on
   agent_stop / before container reaps). Killing the container or the
   worker loses unsaved turns; a clean `podman stop` lets the exec drain.
5. **One daedalus Agent per process.** hive-agent hosts exactly one
   Agent inside its container, so the old hub-side serialization
   machinery (agent_exec_mutex, AgentRegistry) is gone. daedalus still
   shares global state internally -- never run two Agents in one
   hive-agent process.
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
11. **WorkerChannel::send() never touches a socket under the channel
    mutex.** Each worker connection has an outbound queue + dedicated
    writer thread (2026-08-30: a blocking sendText to a NAT-dead worker
    held m_ for minutes and wedged every other worker's connect).
    Disconnect cleanup closes the socket to unblock a stuck writer and
    erases conns_ only if the entry is still that connection (same-name
    reconnect race).
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
hive-server/src/            ChatService (threads/fan-in/live turns),
                            HubRouter (task dispatch), WorkerChannel (WS),
                            WorkerStore; stores: Agent/Thread/Approval/
                            Routine/Auth/Secret/Connector/ExecAudit;
                            Files, Schedule, Vault. NO AgentHost/LLMStore --
                            brains and LLM creds live on workers.
hive-server/include/hivemind/routes/   REST registrars (h+cpp side by side)
hive-server/data/           Containerfile.hive-agent (+browser variant),
                            hive-screen.sh, hive-server.service
hive-client/src/            HiveApplication, HiveWindow, ApiClient
hive-worker/src/            main.cpp (WS client loop), runner (podman ops)
web/dist/                   zero-build web dashboard (pages/ per view)
tests/                      one binary per store/subsystem (20 tests)
```

State at runtime (`~/.local/share/hivemind/`): `agents.db`, `threads.db`,
`approvals.db`, `routines.db`, `auth.db`, `secrets.db`,
`connectors.db`, `exec_audit.db`, `workers.db`, `skills/` (global library),
`workspace/` (shared company volume). Worker rigs keep their state at
`~/.local/share/hive-worker/`: `workspace/` (ONE shared mount for all
containers on that rig), `home-<agentid>/` (per-agent homes),
`models.json` + `secrets.json` (LLM config, 0600), `env/<agentid>.env`
(rendered spawn env), `skills/` (synced library).

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
`daedalus::BaseTool`, register in `hive-agent/src/main.cpp` (alongside
MessageAgentTool) with a unique name, bump the hive-agent image. **Add a test:** `tests/test_foo.cpp` + `hivemind_test(test_foo)`.
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

## 6. Agent runtime details (hive-agent, in-container)

These are implementation facts that change rarely but trip up new work:

### Process model

- Each agent = ONE podman container on its worker (`hive-<agentid>`),
  ONE `podman exec -i` session running `hive-agent /workspace`, ONE
  daedalus Agent. The worker's fleet layer owns the stdio pipes.
- NDJSON protocol on stdio -- ops in (`task`, `approval_resp`, `cancel`,
  `shutdown`), events out (`ready`, `delta`, `message`,
  `approval_request`, `send_agent_message`, `status`, `log`). The exact
  ev -> channel-op mapping table is in `docs/hive-worker.md`.
- Config arrives as env (HIVEMIND_API_HOST / HIVEMIND_API_KEY /
  HIVEMIND_CHAT_ENDPOINT / HIVEMIND_MODEL / HIVEMIND_MAX_CONTEXT /
  HIVEMIND_TEMPERATURE / HIVEMIND_AUTO_APPROVE / HIVEMIND_MCP_SERVERS),
  rendered by the worker from its own models.json + secrets.json.

### Tools

- daedalus built-ins (shell, file, web tools) run IN the container --
  the agent's sandbox is real, no host tunneling.
- Custom: `message_agent` (async delegation: posts to the auto-created
  pair thread; fan-in wakes the asker. Do NOT make this synchronous).
- MCP servers from the spawn frame are seeded into the agent's
  daedalus.db at startup and spawned by daedalus as local child
  processes in the container.

### Approvals

- With `auto_approve` off, daedalus' approval sink hook blocks the turn
  until an `approval_resp` arrives on stdin (10-minute safety timeout
  denies). The request rides the channel as `approval_req`; the user
  resolves it in the client (or web) via the hub's approvals API.

### Thread/context model

- Threads fix participants at creation; there is no "add agent to
  existing thread" flow.
- Conversation history lives in daedalus' messages.json on the shared
  /workspace volume (`.daedalus/daedalus-<Name>/`), restored on every
  hive-agent start. ChatService fan-in sends ONLY the new message; do
  not prepend backlogs.
- Deleting an agent cascades: solo threads deleted, group threads keep
  going with a system note; the worker reaps the container
  (`agent_remove`) and its home dir.

### Skills

- The hub streams its canonical library to workers (skill_begin /
  skill_file / skill_end / skill_remove); the worker bind-mounts it
  read-only at daedalus' global skills dir in every container.
- `/workspace/.agents/skills/` is project-local and shared by every
  agent on the worker (auto-scanned, zero mounts).
- Local skills are per-agent, learned per-agent, owned per-agent.

## 7. Current status (0.5.3+, 2026-08-31)

Phases 0-31 done and user-verified (see Plan.md). Workers connect over
**wss://** through nginx (the OmniNet idle-close bug it exposed is fixed
upstream, rule 13). LLM config is worker-owned (rule 2). WorkerChannel
has per-connection outbound writer queues (rule 11).

Known operational trap (2026-08-30, docs/containers.md): if the worker's
pasta process dies, ALL container networking dies -- agents look ready
but every LLM call retries forever. `pgrep pasta` as the worker user;
recovery = stop ALL containers, then start them.

**Auth:** all `/api/*` calls need `Authorization: Bearer <key>`; the
master key prints once at first start. Worker channel auth: `hmwk_` keys
from `enroll-worker` (SHA-256 hashed in workers.db).
