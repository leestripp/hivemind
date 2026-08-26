# AGENTS.md — HiveMind 2.0 (0.5.0)

> Quick-start reference for AI coding agents. Read this before touching code.
> Deeper rationale: `docs/design.md`. Roadmap & phase status: `Plan.md`.

---

## 1. What this is

HiveMind 2.0 is a **self-hosted team of always-on AI agents for Linux**
(Ubuntu 26.04 target; no Windows/macOS). Persistent named teammates
(`daedalus::Agent`) you message in threads; they run commands in rootless
podman containers as user `hive` (passwordless sudo) — on the server by
default or on any enrolled `hive-worker` machine — delegate to each other,
wait for human approval when gated, and fire scheduled routines. The 1.x
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
| HTTP/WS | **OmniNet** (FetchContent, `main`) — the ONLY HTTP client/stack |
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
(shared company volume).

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

## 6. Current status (0.5.0, 2026-08-26)

Phases 0–30.4 done and user-verified (see Plan.md). Open: **30.5 TLS**
(hard prerequisite before workers leave localhost/LAN — Caddy on the
server, worker pins the CA), screen cold-start readiness race, packaging
follow-ups (first real Flatpak build).

**Auth:** all `/api/*` calls need `Authorization: Bearer <key>`; the master
key prints once at first start. Worker channel auth: `hmwk_` keys from
`enroll-worker` (SHA-256 hashed in workers.db).
