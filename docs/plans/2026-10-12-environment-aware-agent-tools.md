# Plan — One hive-agent, Two Runtimes: environment-aware Agent Tools

Status: **PROPOSED 2026-10-12** — awaiting Lee's sign-off before code.
Triggered by Lee: "get the hive-agent working on both podman container
and bare metal; a lot of the Agent Tools don't work correctly. Do we
need 2 versions, or can the tools be aware of the agent type?"

## 1. Decision: ONE binary, environment-aware tools

**We do not need two versions of hive-agent.** The divergence between
the two worlds is *environmental* (paths, port publishing, screen,
browser backend), not behavioural — the same binary is already spawned
both ways today (`podman exec … hive-agent /workspace` vs
`sudo -u hivex-<name> /usr/local/bin/hive-agent <ws>`), speaks the same
stdio NDJSON protocol, and registers the same tools. Forking the binary
would double every future tool fix for the sake of ~5 tools that
actually care.

The real defect: **nothing tells the agent (or the worker's file ops)
which world it is in.** `build_env_file()` emits a byte-identical env
for both spawn types — down to `HIVEMIND_SKILLS_DIR=/skills`, a path
that only exists inside containers. Every bug below is a consequence of
that missing signal plus hard-coded container paths on both sides.

## 2. What's actually broken on host agents (audit, 2026-10-12)

Severity: **BROKEN** = tool errors or silently does the wrong thing;
**DEGRADED** = works by accident on ubuntu-pc only; **N/A** = feature
is container-only by nature.

| # | Area | State | Root cause |
|---|------|-------|-----------|
| 1 | Agent type signal | **BROKEN (missing)** | `fleet.cpp build_env_file()` emits no runtime/type marker; identical env for both spawn types. |
| 2 | `share_file` (send) FROM a host agent | **BROKEN** | Worker `share_create_req` resolves the source against the *container staging* workspace (`~/.local/share/hive-worker/workspace`); the host agent's files live in the REAL workspace (`--host-workspace-root`). The 2026-10-12 QueenBee fix only symlinked `incoming/`+`shared/` (the fetch direction). |
| 3 | `attach_file` FROM a host agent | **BROKEN** | (a) hive-agent `main.cpp` hard-codes the `/workspace` prefix (strip + `fs::path("/workspace")/rel`); (b) the hub fetches the blob via `file_fetch`, which the worker resolves against the staging workspace — the host agent's `attachments/` dir is never seen. |
| 4 | `send_file` (user → host agent) | **BROKEN** | `file_push` writes into the staging workspace; the host agent's cwd is the real workspace. File lands where the agent never looks. |
| 5 | Workspace file browser (`file_list`) | **BROKEN** | Same staging-vs-real split: the hub shows the container staging tree for a host agent. |
| 6 | Browser tools | **BROKEN** | Driver is image-baked at `/usr/local/bin/browser_driver.py` (`Containerfile.hive-agent`); absent on bare metal → "browser driver not installed in this container". Even if installed, the driver + `BrowserTools` assume in-container Xvfb/headless stack and cwd `/workspace`. |
| 7 | Service sharing (`share_service`) | **BROKEN** | `HIVEMIND_SERVICE_HOST/_PORT_BASE/_PORT_COUNT` are injected only as `podman -e` create-time args (`service_env_args()`), so host agents report "not configured". Ironic: host agents are the *best* case — they bind real host ports; the URL mapping is the identity. |
| 8 | Skills library tiers | **DEGRADED** | `/skills` (synced RO library) is a bind mount; `/usr/local/share/hive-agent/skills` (built-ins) is image COPY. Neither exists on host → host agents silently lose both library tiers; only the workspace tier (`.agents/skills/`) works. |
| 9 | Screen / computer-use | **N/A — fail dirty** | `screen_ctl` is podman+Xvfb+noVNC+6080. Hub `ScreenRoutes` will still try for a host agent and fail ungracefully (worker attempts container ops on a non-container). |
| 10 | Admin exec (`/api/agents/:id/container/exec`) | **BROKEN (dangerous)** | Worker `run_container_exec` calls `ensure_container(agent)` unconditionally — an admin exec against a HOST agent would CREATE an unwanted container named `hive-<id>`. |
| 11 | `boot.sh` service auto-restart | **BROKEN** | The hook runs only from the container (re)create path in `ensure_container`; host spawns never trigger it → agent services don't come back after respawn. |
| 12 | memory/cpus caps | N/A, documented | podman-only by nature; already documented in `docs/host-agents.md`. |
| 13 | A2A server port | **audit item** | Containers isolate ports per netns; multiple host agents on one rig share the host netns — a fixed A2A listen port would collide. (A2A is opt-in; verify before enabling for host agents.) |

Items 2–5 are one systemic bug: **the worker resolves all file ops
against the container staging workspace, but host agents live in the
real workspace root.**

## 3. Design

### 3.1 Runtime descriptor (worker → agent), `fleet.cpp build_env_file()`

The worker knows `spec_.host_agent` at spawn; make it say so:

```
HIVEMIND_RUNTIME=container|host        # absent on old workers = container (back-compat)
HIVEMIND_WORKSPACE_ROOT=<abs path>     # container: /workspace
                                       # host:      host_workspace(name) (the --host-workspace-root)
HIVEMIND_SKILLS_DIR=<...>              # container: /skills (unchanged)
                                       # host:      <worker_data_dir>/skills (the REAL synced library path)
```

For host agents ALSO emit the service block with an **identity
mapping** — reusing the same per-agent port registry
(`service_ports.txt`) so host and container agents on one rig never
collide:

```
HIVEMIND_SERVICE_HOST=<service_host_addr()>
HIVEMIND_SERVICE_PORT_BASE=<allocated base>
HIVEMIND_SERVICE_PORT_COUNT=<count>
HIVEMIND_SERVICE_CONTAINER_BASE=<allocated base>   # == PORT_BASE → url is host:port verbatim
```

`ServiceTools::service_url()` already computes `base + (port -
container_base)`; with container_base == base the URL becomes
`http://<host>:<port>` unchanged, and the "bind inside your range"
validation keeps working — the range is just real host ports now.

### 3.2 Agent-side: `RuntimeContext` (hive-agent)

- Parse once in `main.cpp`: `{ runtime (container|host), workspace_root }`
  from the env above; defaults `{container, "/workspace"}` when absent.
- Thread it into the tools that hard-code paths:
  - `attach_file` / `share_file` / `fetch_shared_file` / prompt text in
    `main.cpp` — replace the `"/workspace"` literals with
    `workspace_root`; render tool DESCRIPTIONS from it too, so the LLM
    sees truthful paths (`/workspace/...` vs the real root).
  - `ServiceTools` — manifest path already honours `HIVE_WORKSPACE`;
    align it to `workspace_root` (set `HIVE_WORKSPACE` from the same
    source or read the new var directly).
  - `BrowserTools` — keep the graceful "not installed" error in this
    phase (host browser backend is Phase D, below).
- Path confinement rules are unchanged: everything still resolves
  *relative to the workspace root* with `..` rejected — the root just
  isn't a literal any more.

### 3.3 Worker-side: per-agent workspace resolution

Add `workspace_for(agent_id)` (fleet knows each spec): host →
`host_workspace(name)`, container → staging workspace. Apply to:

- `share_create_req` — resolve the SOURCE against the sender's own
  workspace (fixes #2). Staging destination stays `shared/<id>/` in the
  staging workspace (transport area; the QueenBee symlinks already
  expose it to host agents).
- `file_fetch` / `file_push` / `file_list` — add an `agent` field to
  these hub→worker frames (the hub knows the agent for attachment
  fetch, send_file and the file browser) and resolve per-agent
  (fixes #3b, #4, #5). Missing `agent` = staging = today's container
  behaviour (back-compat with an old hub).
- `exec` op — if the target spec is a host agent, run via
  `sudo -n -H -u hivex-<name>` in the host workspace (streaming variant
  of the existing `run_as_user`); **never** call `ensure_container`
  for a host agent (fixes #10). No new sudo surface: reuses the
  existing provisioned grant.
- Host spawn/respawn — trigger `<ws_root>/.hivemind/boot.sh` detached
  as the hivex user with the same `boot.log` semantics as containers
  (fixes #11).

### 3.4 Hub-side gates

- `ScreenRoutes`: `host_agent` → clean `409 screen is container-only`
  (fixes #9 failing dirty). A real-desktop capture mode is explicitly
  OUT OF SCOPE (security review needed before an unsandboxed agent
  shares the rig's physical screen).
- `ContainerRoutes` exec: works unchanged once the worker gains the
  host path (3.3); no route change needed.

### 3.5 Security invariants (do not regress)

- `HIVEMIND_RUNTIME` is informational only. Every enforcement
  (screen refusal, workspace choice, exec path) is decided hub/worker-
  side from the roster `host_agent` flag — an agent claiming to be a
  container gains nothing.
- The host workspace root comes from worker config
  (`--host-workspace-root`), never from agent-supplied data.
- `..` rejection and share-addressing rules apply identically on both
  roots; the 50 MB caps stay.
- No new sudoers grants; the exec host path reuses the provisioned
  `worker → hivex-<name>` rule.

## 4. Work items

**Phase A — plumbing (worker + hub frames)**
1. `build_env_file()`: emit `HIVEMIND_RUNTIME`, `HIVEMIND_WORKSPACE_ROOT`,
   host-aware `HIVEMIND_SKILLS_DIR`, host service env (identity mapping).
2. `workspace_for(agent_id)` + use in `share_create_req`.
3. `agent` field on `file_fetch`/`file_push`/`file_list` frames
   (hub `WorkerChannel` + worker handlers), per-agent resolution.
4. Worker `exec` op: host path via sudo; guard `ensure_container`.
5. Host spawn: `boot.sh` hook.
6. Hub `ScreenRoutes` 409 for host agents.

**Phase B — agent runtime awareness (hive-agent)**
7. `RuntimeContext` parse + plumb to tools.
8. De-literal `/workspace` in `main.cpp` tools + descriptions.
9. `ServiceTools` identity-mapping verification on host.

**Phase C — verification + docs**
10. Unit tests (below) + full ctest.
11. Live verification on ubuntu-pc with QueenBee (host agent) and one
    container agent (checklist in §5).
12. Update `docs/host-agents.md` (capability matrix) + AGENTS.md tip.

**Phase D — optional, later (own plan if wanted)**
13. Host browser backend: provisioning installs `browser_driver.py` +
    deps; driver gains a host mode (headless system Chromium over CDP;
    no Xvfb needed — or the real DISPLAY with explicit approval).
14. A2A port allocation for multiple host agents on one rig.

## 5. Verification

Unit/ctest (new):
- worker: `workspace_for` resolution both types; share_create src
  resolution against a host root; exec refuses container creation for
  host specs.
- agent: `RuntimeContext` env parsing (absent → container/"/workspace");
  attach/share path logic under a non-/workspace root; ServiceTools
  identity URL math (`container_base == port_base`).
- hub: screen routes → 409 for host agents (route test).

Live (ubuntu-pc, after deploy):
1. QueenBee (host) → `share_file` → container agent fetches: OK.
2. Container agent → `share_file` → QueenBee fetches: OK (regression
   check of the QueenBee symlink fix).
3. QueenBee `attach_file` → user sees the image in chat.
4. User `send_file` → QueenBee sees it in her workspace.
5. QueenBee `share_service` on a real port → URL reachable from LAN.
6. Respawn QueenBee → her `boot.sh` ran (`.hivemind/boot.log`).
7. Screen route for QueenBee → clean 409, not a worker error.
8. Admin exec against QueenBee → runs as `hivex-queenbee`, no container
   created (`podman ps` unchanged).
9. Full ctest green on both rigs; container agents unchanged (Bob
   smoke turn incl. attach_file + share_service).

## 6. Live validation (2026-10-12, QueenBee — the one host agent)

Eight-point audit sent to QueenBee's DM; her exact outputs confirmed
the audit predictions:

| Finding | Live result |
|---|---|
| #1 no runtime signal | CONFIRMED — `HIVEMIND_RUNTIME`, `HIVEMIND_WORKSPACE_ROOT`, all `HIVEMIND_SERVICE_*` UNSET; `HIVEMIND_SKILLS_DIR=/skills` exported but nonexistent. |
| #2 share_file send | CONFIRMED BROKEN — `{"error":"not found: /workspace/qb-share-test.txt"}` though the file existed in her real workspace. Inbound fetch works (QueenBee symlink fix). |
| #3 attach_file | CONFIRMED BROKEN **and worse than predicted**: the tool returns `{"status":"success"}` (the copy into her real `/workspace/attachments/` works) but the hub-side fetch misses silently — her reply landed with `attachments:[]`. Silent failure, not an error. |
| #6 browser | CONFIRMED — `browser driver not installed in this container` (driver is image-baked). |
| #7 share_service | CONFIRMED — clean "not configured" error; env unset at source. |
| #8 skills tiers | CONFIRMED — exactly 1 skill visible (her own hand-built workspace one); `/skills` and `/usr/local/share/hive-agent/skills` both absent. |

New sub-findings from the live run:
- QueenBee's self-model was wrong twice: she called herself a
  "container" and prescribed "recreate my container" as the fix for
  #6/#7 — meaningless for a host agent. Calibration correction sent.
  (Host agents need their runtime explained in the system prompt or
  they will keep prescribing container remedies.)
- Her workspace root IS literally `/workspace` on ubuntu-pc
  (RAID mount), so agent-side hard-coded `/workspace` paths
  accidentally work there — the agent-side half of #3 is latent, the
  worker/hub-side half is what actually broke. Both halves still get
  fixed (other rigs won't share the coincidence).

## 7. Implementation status (2026-10-12, commit 75d5ab1)

**Phase A + B + C COMPLETE** (deploy 2026-10-13): built warning-clean,
128/128 ctest green, pushed to all three remotes, both rigs rebuilt
(images + binaries), hub + workers restarted, all 5 containers on new
images (binary proof: HIVEMIND_RUNTIME in every hive-agent), QueenBee
respawned as host agent with the runtime descriptor env.
**Live re-verification: 8/8 PASS** (QueenBee re-audit 2026-10-13):
env descriptor, share_file (byte-intact cross-agent fetch by Bob),
attach_file (three-way verified after a setfacl fix on staging
attachments — provisioning helper already covers this for fresh
provisions), skills library visible, bash, attachments symlink, hub
screen/start 409, runtime calibration. Deploy notes: droplet podman
wedged mid-deploy (searxng quadlet's generated `podman rm -i`
ExecStopPost blocks on stdin holding the storage lock; fix sequence in
deploy-hivemind skill Field lessons #5). Scout re-pinned (screen port
accepts, browser_driver title proof). Delivery sweep clean.
Changes: worker runtime descriptor env (RuntimeEnv/runtime_env_lines,
unit-tested in test_fleet), per-agent file-op roots (agent_workspace,
share source, attachment promote, file_fetch/file_list agent field),
host exec via sudo, host boot.sh hook, provisioning attachments
symlink, hub screen/start 409, agent-side workspace-root plumbing +
runtime paragraph in the system prompt. Docs: docs/host-agents.md
"Runtime descriptor env". Remaining (Phase C/D): deploy + live
re-verify; browser/screen questions below still open.

## 8. Open questions for Lee

1. Host browser (Phase D): wanted at all, and if so — headless-only,
   or may an agent drive the rig's real desktop session (needs the
   accessibility conversation first)?
2. When `--host-workspace-root` is unset the host workspace falls back
   to the hivex home dir — file ops then confine to that home. Keep,
   or make the root mandatory for host agents?
3. Screen for host agents: confirm "never" vs "someday, with a real-
   desktop capture design".
