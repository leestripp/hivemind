# HiveMind — Next Steps / Working Notes

> Status: 2026-08-08 — all 8 node types live-verified (agents against
> MiniMax-M2.7). UI modular (ES modules, Nodes-first). Chat panel removed —
> external agents drive HiveMind via the public API (`docs/api.md`).

## Quick project state

- **Build:** `cmake --build build -j$(nproc)` — clean.
- **Tests:** `ctest --test-dir build` → 4/4 pass (LoopEngine, node ports,
  daedalus smoke, graph runner e2e).
- **Server:** `./build/hivemind --port 8421` (dev).
- **Docs:** `AGENTS.md` (agent quick-start), `docs/design.md` (architecture),
  `docs/api.md` (full REST reference).
- **History:** completed milestones live in `git log` — this file tracks
  only what's ahead.

## What to do next (priority order)

0. **Agent-loop judge quality** — the exit path is verified; exercise the
   continue path in a real refinement workflow and tune the judge prompt
   (MiniMax occasionally returns an empty verdict; the executor falls
   back to text parsing, then to exit).
1. **WebSocket run streaming** (`/ws/runs`) — replaces HTTP polling of
   `GET /api/runs/:rid/output`. Use OmniNet's `registerProtocolSwitch`
   (public API; do NOT patch OmniNet).
2. **Skip-if-running trigger policy** — scheduler skips a fire when the
   graph already has a `running` run (~15 lines: `has_running_run(gid)`
   check in the scheduler loop).
3. **Run-history page** — UI list of past runs per workflow
   (`runs` table already has everything).

## Node-type status (all shipped)

| Node type | Backend | Status |
|-----------|---------|--------|
| `input` | `InputExecutor` | ✅ end-to-end |
| `prompt` | `PromptExecutor` | ✅ end-to-end |
| `output` | `OutputExecutor` | ✅ keys by `output_name` |
| `log` | `LogExecutor` | ✅ end-to-end |
| `merge` | `MergeExecutor` | ✅ concat (interleave = future) |
| `loop_start`/`loop_end` | `LoopEngine` | ✅ feedback + loop_count |
| `agent` | `daedalus::Agent` | ✅ live vs MiniMax-M2.7 |
| `agent_loop_start`/`agent_loop_end` | daedalus judge + `loop_decision` tool | ✅ e2e verified (exit path) |

## What *not* to do

- Don't re-add a chat panel — external agents use the public API.
- Don't patch OmniNet or daedalus — use their public APIs.
- Don't add new node types until WebSocket streaming is in.
