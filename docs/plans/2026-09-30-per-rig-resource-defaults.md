# Plan — Per-Rig Resource Defaults (Phase 70: kill the global 4g/2 constants)

Status: **PROPOSED 2026-09-30** — awaiting Lee's sign-off before code.
Triggered by Lee's (correct) criticism: defaults sized for the weakest
rig silently apply to the strongest. Phase 69 made memory/cpus
configurable PER-AGENT (Bob 32g/12, Forge 16g/8, droplet 4×1536m —
all live), but left the DEFAULT as a global compile-time constant
(`4g` / `2` in hive-worker/src/runner.cpp) chosen with the droplet in
mind. Any agent without an explicit override on ubuntu-pc — a
16-core/32-thread, 60Gi rig — still gets 2 CPUs. That constant should
never have survived Phase 69; this plan removes it.

## 1. Problem

- **Defaults are global, rigs are not.** `effective_limits()` falls
  back to literal `"4g"` / `2` for any agent with no roster override
  and no pin. Right on a 3.8Gi droplet, absurd on a 60Gi workstation.
- **The hub is blind to rig capacity.** Worker hello caps advertise
  `gpu` only. Nothing anywhere knows a rig has 32 threads / 60Gi, so
  no UI hint, no validation, no sensible derived default is possible.
- **No per-worker knob.** Hardware config on a worker is today limited
  to models/secrets. "What should an unspecified agent get ON THIS
  RIG?" has no answer except the compiled constant.
- **Visibility gap.** Effective limits (after roster → pin → default
  resolution) are only knowable via `podman inspect` on the rig.

## 2. Agreed design (locked unless Lee says otherwise)

Precedence (highest wins), all layers already exist except 2 and 4:

1. **Per-agent roster override** (Phase 69 — DONE, unchanged).
2. **Per-worker defaults file** — `defaults.json` in the worker dir
   (`~/.local/share/hive-worker/`), worker-owned hardware config in
   the models.json/secrets.json tradition: the hub NEVER persists it,
   relay-only routes forward to the worker. Shape:
   ```json
   {"memory": "16g", "cpus": 8, "shm_size": "1g"}
   ```
   Any key absent → fall through to layer 3.
3. **Derived factory default** (replaces the bare constants): computed
   once at worker start from the LOCAL machine —
   - `memory = clamp(host_ram / 4, 1536m, 16g)`
   - `cpus   = clamp(host_threads / 4, 2, 16)`
   Sanity: droplet (3.8Gi/2T) → 1536m/2 (unchanged behaviour);
   ubuntu-pc (60Gi/32T) → 15g/8.
4. **Capacity advertisement**: worker hello `caps` gains
   `"resources": {"cores": N, "threads": N, "ram_bytes": N}` (sysinfo
   / /proc/cpuinfo). Hub stores it in the existing workers.caps JSON —
   no schema change — and `GET /api/workers` already returns caps, so
   both UIs can show rig capacity and warn when an agent's cap exceeds
   the rig's RAM. Soft warn only (UI badge + API `warning` field);
   never a 400 — caps are limits, overcommit is legitimate.
5. **Effective-limits visibility**: worker includes the RESOLVED
   limits (`memory`/`cpus` after the full precedence chain) in
   `agent_status` frames; hub caches the latest per agent (in-memory,
   like live state — not persisted) and exposes it on
   `GET /api/agents` as `effective_memory`/`effective_cpus`. UIs show
   e.g. "32g · 12 cpu" on the roster card.
6. **shm_size stays worker-level only** (defaults.json key + 1g
   factory default). No per-agent shm knob until a real case appears
   — YAGNI.

Hub stays dumb (AGENTS.md rule 3): no hub-side defaults, no hub-side
capacity math beyond storing/displaying what workers volunteer.

## 3. Worker changes (hive-worker)

- `sysinfo()` + cpu count probe → `host_resources()` in runner.cpp
  (pure, testable via injected values).
- `derived_defaults()`: the clamp formulas of §2.3.
- `load_worker_defaults()`: read `worker_dir()/defaults.json`
  (nlohmann, tolerant — missing/garbage file = all layers fall
  through; log once).
- `effective_limits()` final chain: agent arg → pin → defaults.json →
  derived. The literal `"4g"`/`2` constants are DELETED.
- Hello frame: `caps.resources` from `host_resources()` (main.cpp,
  next to the existing gpu cap).
- `agent_status` frames gain `memory`/`cpus` (resolved values) —
  main.cpp where status is already emitted.
- New channel ops `defaults_get` / `defaults_set` (mirror the
  models/secrets relay handlers in main.cpp; `defaults_set` rewrites
  the file 0600 and re-pins nothing — pins are per-agent and
  unaffected; takes effect on the next ensure_container call, and
  `podman update` reconcile applies it live).

## 4. Hub changes (hive-server)

- Relay-only routes (admin-scoped, persist NOTHING hub-side — same
  contract as models/secrets):
  - `GET /api/workers/:name/defaults` → relay `defaults_get`
  - `PUT /api/workers/:name/defaults` → relay `defaults_set`
    (validate: memory passes the Phase 69 size check, cpus ≥ 0)
- `GET /api/agents` rows gain `effective_memory`/`effective_cpus`
  from the latest agent_status cache (`""`/0 when never reported).
- WorkerStore: nothing to change (caps already a JSON blob); confirm
  the resources object round-trips.

## 5. Client + webUI

- Roster cards (GTK + web): show effective caps when known
  (`32g · 12 cpu`), dimmed "(default)" suffix when the agent has no
  roster override — makes the droplet-vs-workstation difference
  VISIBLE, which is what Lee asked should have raised a flag.
- Agent editor (both): placeholder text shows the RIG's default
  ("worker default (15g)" instead of a hardcoded "4g" string) using
  the selected worker's advertised/known defaults.
- Workers page (web) / worker rows: show `threads · RAM` from
  caps.resources; warn badge when Σ(agent memory caps) > rig RAM.

## 6. Tests

- `test_fleet` / new pure tests: clamp formulas at the boundaries
  (droplet-sized, workstation-sized, 1-core VM, 1Ti RAM),
  `defaults.json` precedence (file beats derived, agent beats file),
  garbage-file tolerance.
- `test_roster_routes`-style: defaults relay routes (fake worker
  channel, the pattern from secret/models route tests), effective
  fields on GET /api/agents.
- `test_worker_channel`: hello frame carries resources.
- Full ctest `--timeout 60 -j$(nproc)` before ANY deploy (standing
  rule).

## 7. Deploy (when implemented)

Limits/defaults-only deploy — NO image builds, NO container
recreation (Phase 69 `podman update` reconcile does it live):
1. hub rebuild + restart (droplet), worker rebuild + restart (both
   rigs, 2b net-probe after each restart — pasta lesson stands).
2. Write `defaults.json` per rig: droplet `{"memory":"1536m","cpus":1}`
   (2 vCPUs shared by 4 agents — 1 each by default, burst via
   per-agent override), ubuntu-pc `{"memory":"16g","cpus":8}`.
3. Verify: `podman inspect` caps per rig, journal
   "limits reconciled" lines, `GET /api/agents` effective fields,
   browser-driver probe per rig, agent check-in per rig, client
   smoke test. Full 5b/5c/5d protocol per the deploy skill.

## 8. Docs

- `docs/hive-worker.md`: defaults.json + derived formulas + caps.
- `docs/api.md`: defaults routes, effective_* fields.
- `AGENTS.md`: replace any "hardcoded 4g/2" mental model; add the
  precedence chain to Practical Tips (it is exactly the kind of fact
  an agent must not have to rediscover).
- Deploy skill: note that defaults changes are live-applied.

## 9. Out of scope (conscious)

- Per-agent shm_size, memory-swap tuning, CPU pinning/affinity,
  per-agent io limits. Real cases first.
- Hub-side enforcement of per-rig totals (admission control). Soft
  UI warnings only; overcommit is a legitimate policy.

## 10. Acceptance

- Delete every literal default limit from hive-worker; an agent with
  no override on ubuntu-pc lands on 15g/8 (derived) or 16g/8 (file),
  on the droplet 1536m/2 (derived) or the file values.
- `GET /api/agents` shows effective caps; both UIs display them.
- Droplet behaviour unchanged unless its defaults.json says so.
- 116+/116+ ctest green; fleet verification protocol clean.

---

# Plan: OmniNet maxBufferSize rollout — update every consumer

Date: 2026-10-07 · Requested by: lee · Status: COMPLETE (except splice, blocked — see §4)

## Background

Long STT recordings (>~31 s) in hive-client-voice died silently: ChimpFlow's
OmniNet HTTP server capped request handling at ~1 MB (`Buffer too large,
returning false` in the journal at totalReceived=1,016,121). Whisper direct
handles the same 60 s file in 4 s — the gateway was the wall.

OmniNet v1.0.5 (commit f70af1c, now origin/main) makes the socket buffer cap
configurable at every layer, default unchanged (16 MB):

- `Socket::setMaxBufferSize()` per-instance
- `HttpServer::Config.maxBufferSize` → every accepted connection
- `ClientConfig.maxBufferSize` → every request socket (plain + TLS)
- config file key `server.maxBufferSize`
- Also includes bug fixes #7–#9 (upload ceiling, send span cap, API key
  prefix split) and Happy-Eyeballs racing connect.

## Consumer inventory

| Project | OmniNet source | Needs code change? |
|---|---|---|
| Services/ChimpFlow | Fetch main + force-ff at configure | **Already done** in src/main.cpp (`maxRequestSize=24MB`, `maxBufferSize=32MB`) |
| AI/Hive-Mind (hive-server, hive-worker, hive-agent, hive-client, hive-client-voice) | Fetch main + ff at configure (OMNINET_AUTO_UPDATE) | No — defaults are correct (hub bodies small; voice client sends ≤3.8 MB, receives ≤16 MB) |
| AI/ApolloCore | Fetch main (shallow) | No — build-check only |
| AI/daedalus | daedalus/cmake/FetchOmniNet.cmake | No — build-check only (live agent runtime: build, do NOT install/restart) |
| Apps-GTK4/splice | Fetch main (shallow) | No — build-check only |
| Games/vortxGE | **Pinned SHA b6e8084** "for reproducible builds" | FLAG to lee — do not bump a deliberate reproducible-build pin unasked |
| Websites/lscppdev.com | Fetch main (shallow) | No — build-check only |
| Websites/seeoztogether.com | Fetch main + build-time sync target | No — build-check only |

## Execution steps

### 1. OmniNet itself — verify the new feature
- [x] Build OmniNet's own tests in /projects/Websites/OmniNet
- [x] Run test_buffer_protection + test_config (the new coverage) and full suite

### 2. ChimpFlow — the actual STT fix

DONE: 60s upload HTTP 200 in 5s, 100s in 8s (both previously dropped
at ~1MB). Service active on the new binary.
- [x] Reconfigure + rebuild (force-ff pulls OmniNet v1.0.5)
- [x] Restart the chimpflow user service
- [ ] Verify: POST 60 s / 100 s / 120 s 16 kHz WAVs to :8090/v1/audio/transcriptions
      → all must return HTTP 200 (previously dropped at ~1 MB)

### 3. Hive-Mind — rebuild against v1.0.5
- [x] Reconfigure (auto-ff pulls OmniNet to f70af1c — confirm the
      "[hivemind] OmniNet updated" status line)
- [x] Build all targets warning-clean
- [x] Full ctest suite green
- [x] Install hive-client-voice to ~/.local/bin
- [x] Commit (Plan.md + any changes)
- [x] Deploy hive-server to the droplet via the deploy-hivemind runbook
      (per-rig rebuild, in-container proof, image cleanup)

### 4. Other consumers — build-check against v1.0.5
- [x] ApolloCore: configure + build
- [ ] splice: configure + build — BLOCKED: ffmpeg dev packages
  (libavformat-dev etc.) not installed on this machine; fresh configure
  fails before reaching OmniNet. Awaiting lee's call on apt install.
- [x] lscppdev.com: configure + build
- [x] seeoztogether.com: configure + build
- [x] daedalus: configure + build ONLY (it is the running agent — no install)

### 5. Close-out
- [x] Report: what changed, verification evidence, vortxGE pin decision for lee

## Risks / rollback

- The update is backwards-compatible (defaults unchanged at 16 MB); consumers
  that set nothing behave exactly as before.
- ChimpFlow service restart interrupts TTS/STT for seconds only.
- Droplet deploy follows the documented deploy-hivemind skill with
  post-deploy proofs; rollback = redeploy previous commit.
- daedalus is build-checked only — restarting the live agent is out of scope.

## Out of scope (flagged, not done)

- vortxGE's pinned SHA: deliberate reproducible-build pin; bumping it is
  lee's call.
- Deploying the website projects (lscppdev.com / seeoztogether.com):
  build-verified locally only.

## Addendum: hive-worker LAN-publish fix (found during the fleet deploy)

The full-fleet container recreate exposed a rig-wide HIVE_LAN_PUBLISH
flaw on ubuntu-pc: both agents' fresh containers got 0.0.0.0:8090, so
Bob (Slate Ape) could never start (pasta port collision). Fixed with
the optional HIVE_LAN_PUBLISH_AGENT=<uuid> gate in runner.cpp
(commit 4e8ca14) + the rig's lan-publish.conf drop-in. Verified: Forge
runs without 8090, Bob runs with it, LAN-peer probe HTTP 200, both
containers' networking healthy, worker on the new binary.

## Addendum: post-reboot voice outage — SttEngine retry + prime (2026-10-08)

**Symptom:** after a reboot, hive-client-voice gave no voice. All three
ChimpFlow services (gateway :8090, TTS :8080, whisper :8081) were active
and healthy; a direct WAV round-trip through the gateway succeeded. The
running app had been up ~13 min with zero hub/voice traffic — it had
never connected. A single debug-shell `say` command un-wedged it.

**Root cause:** `SttEngine::start()` did a ONE-SHOT `/healthz` readiness
probe on a detached thread. If the gateway was not answering at that
instant (app launched before the user services finished coming up), the
probe failed silently: `ready_` stayed false, no retry, no log — and the
STT engine was never armed. The app looked idle and healthy but could
never transcribe, so no TTS ever fired. A later unrelated poke
succeeded because the gateway was up by then.

**Fix (this commit):**
- New `SttEngine::readinessLoop(session, captureOnReady)`: probes
  `/healthz` with 5 s backoff, 12 attempts (~1 min), every attempt
  logged to stderr (`hive-voice: chimpflow not responding, retry N/12`).
  On success flips `ready_` and arms capture if the session is live.
- New `SttEngine::prime()`: launch-time warm-up called from the
  VoiceView constructor — probes in the background so a slow gateway is
  picked up without a dead first TALK press. Never touches the mic,
  never surfaces an error.
- TALK (`start()`) now routes through the same retry loop — a tap during
  a gateway restart self-heals instead of dying once.

**Lesson:** a single-shot silent probe at a boundary that can be crossed
while the far side is still booting is a latent outage. Readiness probes
at app/IO boundaries must retry with backoff and log every attempt.

---

# Plan: Agent-to-Agent Secure File Sharing (share_file)

Date: 2026-10-11 · Requested by: lee · Status: IMPLEMENTED (2026-10-11 — ShareStore + hub bridge + worker staging/relay + four agent tools; 128/128 ctest)

## Background

Agents have no clean way to hand a file to each other. They can attach a
file to the USER (via /workspace/attachments + the client upload route),
and agents on the SAME worker can see each other's /workspace by accident
of the shared bind mount — but there is no addressed, access-checked,
cross-worker transfer with notification. Fleet reality: Bob/Forge live on
ubuntu-pc, four agents on the droplet — cross-rig hand-offs are the common
case, not the exception.

Requirements from lee: secure, agent-to-agent, size limit "50MB maybe" →
**50 MB** (matches the existing worker-channel file caps exactly, so no
OmniNet buffer changes are needed).

## Design (fits the architecture rules)

- **File locality** (AGENTS.md): the blob NEVER lives on the hub. The
  sender's worker copies it into a stable staging dir
  (`workspace/shared/<share_id>/<name>`) at share time; the hub stores
  only metadata (ShareStore, SQLite `shares.db`).
- **Transport reuses existing chunked channel ops**: `file_fetch`
  (worker→hub, 192 KB base64 chunks, 50 MB cap) and `file_push`
  (hub→worker, 50 MB cap). Cross-worker fetch is a hub RELAY that holds
  the file in memory transiently only.
- **Bridge pattern** (RoutineBridge/MsgBridge precedent): agent tools
  emit NDJSON events on stdout; the worker stamps the agent id and
  forwards as channel op frames; the hub replies; the worker routes the
  `*_resp` op back to the agent's stdin. No agent-side HTTP.
- **Notification reuses Phase 66**: on share creation the hub posts an
  `agent:<name>` message into the pair thread via
  `ChatService::post_agent_message` — durable delivery, wake, read
  receipt, all already built.
- **Hub stays dumb**: no hub-side FS, no hub-side size math beyond
  trusting the authenticated worker's report (same trust level as
  msg_send_req), relay-only fetch.

## Security model

- Sender path confined to the shared workspace: relative, no `..`,
  regular file, ≤ 50 MB — enforced WORKER-side before any hub traffic.
- Filename sanitized (bare name, `[A-Za-z0-9._-]`, no leading dots).
- Shares are ADDRESSED: exactly one target agent. Fetch authorization is
  hub-enforced — only the target (or the sender) may fetch a share.
- Agent identity is stamped by the worker from its brain-process pipe,
  never taken from agent-supplied frames (same as msg_send_req).
- The fetch destination path (`incoming/<share_id>/<name>`) is revealed
  only AFTER the hub authorizes the fetch.
- Revocation: sender can revoke; hub marks the row revoked and asks the
  sender's worker to delete the staged copy (best-effort).
- Audit: admin-scoped `GET /api/shares` metadata route for diagnosis
  (mirrors `GET /api/deliveries`).
- No user-client access to share contents in v1.
- Out of scope (conscious): sha256 integrity pinning (TLS + TCP already
  protect the hop; note as future hardening), broadcast shares, TTL
  auto-expiry, virus/MIME scanning.

## Pieces

### 1. Hub — ShareStore (new: include/hivemind/ShareStore.h + src/ShareStore.cpp)
- `~/.local/share/hivemind/shares.db`, DeliveryStore pattern: per-op
  connections, `sqlite3_busy_timeout(5000)`, DbUtil helpers.
- Row: id, sender_agent_id, target_agent_id, filename, size_bytes, note,
  worker, rel_path, state(active|revoked), created_at, fetched_at,
  fetch_count.
- Methods: `create_share`, `get_share`, `list_for_agent`,
  `mark_fetched`, `revoke`.

### 2. Hub — HubRouter ops (HubRouter.cpp, alongside handle_msg_send_req)
- `share_create_req` {id, agent, filename, size, note, target, worker,
  rel_path} → validate sender on this worker, resolve target (id or
  name), insert row, post pair-thread notification ("shared a file …
  fetch_shared_file(\"<id>\")"), reply `share_create_resp`.
- `share_list_req` → rows where sender or target == requester →
  `share_list_resp`.
- `share_fetch_req` {share_id} → authorize (target or sender), then on a
  DETACHED thread (fetch_file/push_file block up to 120 s; never stall
  the channel read loop):
  - same-worker: op `share_local_copy` {src rel, dst
    `incoming/<share_id>/<name>`, for_agent} → worker copies in place.
  - cross-worker: `fetch_file(sender_worker, rel)` →
    `push_file(recipient_worker, incoming/<share_id>/<name>)`.
  - `mark_fetched`, then `share_fetch_resp` {ok, path, name, size} to
    the requester's worker → agent stdin.
- `share_revoke_req` {share_id} → sender only; mark revoked; op
  `share_delete` {rel_path} to sender worker (best-effort); resp.

### 3. Hub — admin route
- `GET /api/shares[?agent=<id>]` — metadata list, admin-scoped,
  registered before the static catch-all.

### 4. Worker (hive-worker/src/main.cpp)
- `share_create_req` event: confine path (rel, no `..`, strip
  `/workspace/` prefix), stat (regular, ≤ 50 MB), generate share id,
  sanitize filename, copy into `workspace/shared/<id>/<name>` (0777 dirs
  like file_push), then forward op with {id, filename, size, rel_path}.
  Failure → synthetic `share_create_resp {ok:false,error}` straight back
  to the agent's stdin (no hub round-trip).
- Blind-forward `share_list_req` / `share_fetch_req` / `share_revoke_req`
  with the agent stamp (msg_send_req pattern).
- Route `share_*_resp` ops to agent stdin (extend the forward list).
- New hub→worker ops: `share_local_copy` (confined copy within the
  workspace + ack) and `share_delete` (confined unlink, best-effort).

### 5. Agent (hive-agent/src/main.cpp)
- `ShareBridge` global (MsgBridge clone, id prefix "f"), stdin routing
  for the four `share_*_resp` ops.
- Four tools registered next to MessageAgentTool:
  - `share_file(path, target, note?)` — path is /workspace-relative or
    absolute in-container; ≤ 50 MB; target by name or id; the target is
    woken automatically.
  - `list_shared_files()` — shares to/from you, with ids, sizes, state.
  - `fetch_shared_file(share_id)` — lands at
    `/workspace/incoming/<share_id>/<name>`; result carries the path.
  - `revoke_shared_file(share_id)` — sender only.

### 6. Tests
- `hive-server/tests/test_share_store.cpp`: CRUD, list_for_agent both
  directions, mark_fetched counters, revoke state machine, garbage input.
- Extend an op-level suite if a fake-channel harness exists for
  HubRouter; otherwise store tests + live verification protocol.
- Full ctest green before deploy (standing rule).

### 7. Docs
- `docs/api.md`: GET /api/shares.
- `docs/hive-worker.md` or design.md: the share flow diagram.
- `AGENTS.md`: Messaging section — share_file exists, 50 MB cap,
  addressed-share security model.

### 8. Deploy (lee's go-ahead required)
- deploy-hivemind runbook: hub rebuild (droplet), worker rebuild (both
  rigs), per-rig hive-agent image rebuild + container recreation (the
  tools are baked into the agent image), in-container proofs:
  cross-rig share Bob→droplet agent, same-rig share, oversize rejection,
  wrong-recipient fetch rejection, revocation.

## Risks / rollback

- 50 MB transient in hub memory during a cross-worker relay — fine on
  the droplet (the cap is per-share and concurrent relays are rare).
- A share pointing at a deleted staged file fails at fetch time with a
  clear error (worker file_fetch "not found" path) — no crash.
- Rollback: redeploy previous commit; shares.db is additive-only, no
  schema changes to existing DBs.

---

# Plan: Host Agents — bare-metal hive-agent with sudo (Phase 72)

Date: 2026-10-12 · Requested by: lee · Status: IMPLEMENTED + INTEGRATION-TESTED 2026-10-12 on ubuntu-pc (live rig): full lifecycle green incl. lee's review changes (7e24db7): roster delete AUTO-deprovisions (user/home/sudoers via root helper, home archived to /var/backups), persistent per-agent workspace via worker --host-workspace-root (/projects/hive-agents on ubuntu-pc) NEVER cleaned. sudo-rs -D fix (ecf0f73). TestBot removed at test end; rig clean.

> **⚠ THIS FEATURE RUNS WITHOUT A SANDBOX.** A host agent executes
> directly on a Linux machine as a real unix user WITH passwordless
> sudo. It can install software, modify system files, and — through
> error, hallucination, or prompt injection — DESTROY the machine.
> **Only enable host agents on a machine you don't mind losing.**
> This warning is part of the feature, not documentation garnish: it
> is shown at creation, on every roster/thread surface, and logged on
> every spawn (see §4).

## Background

Every agent today runs inside a rootless podman container (passwordless
sudo as user `hive` INSIDE the container only). That is deliberately
strong isolation — but it also means agents can never do real host
administration: install system packages, manage systemd units, edit
`/etc`, manage disks, or operate the machine the way a human
administrator would.

Lee's ask (2026-10-12): an OPTIONAL agent kind that runs on the host
as a real user with sudo, for machines dedicated to being agent-admin
boxes (disposable VMs, sacrificial rigs). The container path stays the
default and is untouched.

## 1. Design principles (locked unless lee says otherwise)

1. **Container is the default; host is opt-in at TWO levels.** Neither
   level alone can create a host agent:
   - **Worker opt-in**: the worker must be configured
     `allow_host_agents = true` (config file / `HIVE_ALLOW_HOST_AGENTS=1`
     env, default OFF) and advertise `host_agents_capable` in its hello
     caps. A hub asking for a host agent on a non-opted-in worker gets
     a hard refusal, logged.
   - **Per-agent opt-in**: roster flag `host_agent` (bool, default
     false), set at creation or via PATCH — gated by the warning flow
     below.
2. **Provisioning is manual and local.** The hub/worker can NEVER
   create unix users or sudoers entries — that would let anything that
   reaches the hub API mint sudo users remotely. A human runs
   `sudo hive-worker provision-host-agent <name>` ON the rig, which:
   - creates user `hivex-<name>` (locked password, dedicated home
     `/home/hivex-<name>`),
   - drops `/etc/sudoers.d/hivex-<name>` with `NOPASSWD: ALL`
     (validated with `visudo -c`),
   - installs/updates the host-agent binary at
     `/usr/local/bin/hive-agent`,
   - prints the red warning and requires typing the machine's hostname
     to proceed.
   Deprovisioning: `sudo hive-worker deprovision-host-agent <name>`
   removes user (home archived, not deleted), sudoers file, and binary
   reference.
3. **Same brain, same protocol.** The host agent is the SAME
   hive-agent binary speaking the same stdio NDJSON to the worker. No
   hub-side LLM anything, no new message path. Only the SPAWN
   mechanism changes: `sudo -u hivex-<name> -H --set-home    env --env-file F /usr/local/bin/hive-agent` instead of
   `podman exec -u hive -w /workspace …` (fork/exec, no shell, same as
   the container path's discipline).
4. **Home is the workspace.** `/home/hivex-<name>/` plays the role of
   `/workspace`: send_file confinement, share_file staging, skills
   (~/.agents/skills private tier; the synced RO library bind-mount is
   replaced by a plain rsync into a read-only-perms dir). Path
   confinement logic is reused with the home as root.
5. **No per-container luxuries.** No memory/cpu podman caps (optional
   later via `systemd-run` scopes), no noVNC/screen-share (the host
   agent HAS no virtual display; computer-use is container-only), no
   service-port range publish (host agent binds host ports directly —
   documented as a difference).

## 2. The warning system (the heart of the request)

The risk must be unmissable even if the user never reads docs:

- **Creation** (GTK + web + any future client): choosing "Host agent"
  shows a red banner: the ⚠ text above + "Only use this agent on a
  machine you don't mind losing." The confirm button stays disabled
  until the user types `I UNDERSTAND` (web/GTK) — pattern-matched, not
  a checkbox (checkboxes get reflex-clicked).
- **Roster**: host agents carry a persistent red `HOST` badge in the
  GTK roster, web dashboard, and Android roster; the thread header of
  a host agent's DM shows the same badge. No client may hide it.
- **API**: `POST /api/agents` / PATCH with `host_agent:true` requires
  the extra JSON field `"host_agent_ack": "I UNDERSTAND"` — the server
  400s without it, so no client can silently flip the flag. The ack is
  logged to the hub journal with the caller's key name.
- **Spawn**: every host-agent spawn logs a journal line
  `HOST AGENT spawn: <name> on <worker> (sudo, unsandboxed)` so the
  audit trail shows every activation.
- **Approvals default ON**: `require_approval` is forced true at host
  agent creation; disabling it requires the same `I UNDERSTAND` ack.
  Shell commands then park in the existing approval queue by default.

## 3. Changes by component

### hive-worker (the bulk)
- Config: `allow_host_agents` (default false) + hello cap
  `host_agents_capable`.
- `provision-host-agent` / `deprovision-host-agent` subcommands
  (useradd/sudoers/binary install; idempotent; `visudo -c` validated;
  refuse if not root).
- runner.cpp: spawn dispatch — if the spawn frame says
  `host_agent: true`: verify worker opt-in (refuse otherwise), verify
  `hivex-<name>` exists (refuse otherwise: "not provisioned"), then
  fork/exec the sudo path with the same env-file mechanism (0600,
  deleted after spawn) and the same NDJSON stdio pump. No
  ensure_container, no limits update, no screen ops.
- fleet.cpp: drift-check must NOT try to "correct" a host agent into
  a container.

### hive-server (thin, as it should be)
- AgentStore: `host_agent` column (migration, default 0).
- Routes: create/PATCH honor `host_agent` only with the ack field
  (§2); spawn frame carries `host_agent`; refuse targeting a worker
  whose caps lack `host_agents_capable`.
- Roster/threads JSON exposes `host_agent` so clients can badge it.

### Clients (GTK + web; Android badge later)
- Creation dialog warning + type-to-confirm (§2).
- Red `HOST` badges on roster + thread header.

## 4. Security notes (honest ones)

- `NOPASSWD: ALL` means the agent IS effectively root. Sudoers
  whitelisting (`apt`, `systemctl`, …) was considered and rejected as
  false comfort — whitelists are famously bypassable (cp/mv/dd/awk
  escalate trivially). The honest model is: host agent == root ==
  machine is expendable. The mitigations that DO matter:
  - dedicated user (no access to human homes beyond world-readable),
  - approvals ON by default (human reviews shell commands),
  - journal audit of spawns/acks,
  - run on a VM with snapshots (recommended in docs, not enforced).
- The hub never gains the ability to create host agents on a worker
  that hasn't opted in — the two-level opt-in is load-bearing.
- A host agent's container escape concerns are replaced by direct
  exposure: prompt injection from web/tool content can run ANYTHING.
  Docs must say so plainly.

## 5. Tests

- worker: unit — spawn-path selection (host vs container), refusal
  without opt-in, refusal when unprovisioned.
- server: route tests — create/PATCH `host_agent` without ack → 400;
  with ack on non-capable worker → 409; roster exposes the flag.
- integration: full spawn of a host agent on a throwaway user inside a
  DISPOSABLE test VM/container, verify NDJSON turn + sudo works
  (`sudo id -u` → 0 via approvals) + deprovision cleans up.

## 6. Docs

- `docs/host-agents.md`: what it is, the warning, provisioning,
  deprovisioning, recommended sacrificial-VM setup (snapshot before
  first boot), approvals workflow.
- `docs/hive-worker.md`: config flag + spawn path difference.
- `AGENTS.md`: host-agent section (two-level opt-in, ack field,
  badge rule, "hub can never provision users").
- `docs/api.md`: `host_agent` + `host_agent_ack` fields.

## 7. Out of scope

- Running hive-agent as root directly (dedicated user ONLY).
- Sharing the human's own login user (forbidden — dedicated hivex-*).
- Windows/macOS hosts.
- sudoers command whitelisting (false comfort, §4).
- Host-agent screen share / noVNC.

## 8. Acceptance

- Container agents behave exactly as today (no regression in the
  existing suite).
- A host agent can be provisioned, spawned, run a sudo command through
  the approval queue, and be deprovisioned — on a disposable VM.
- Every surface (create/PATCH/roster/thread/journal) shows or records
  the warning path; no API route sets `host_agent` without the ack.
