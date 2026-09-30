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
