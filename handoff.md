# HiveMind handoff (2026-08-30)

You're taking over the HiveMind v2 rebuild. Repo root: `/home/user/projects/AI/Hive-Mind`.

## FIRST: uncommitted work in the tree

`hive-client/src/HiveWindow.{cpp,h}` are MODIFIED, built green, 15/15 ctest green,
**not yet committed**. Two Phase-E-remainder features:

1. **Video attachments** (HiveWindow.cpp): `.mp4/.mkv/.webm/.mov/.avi/.m4v` in chat
   bubbles -> `load_video_attachment()` (same download-to-cache pattern as images) ->
   `show_video_widget()`: paused `Gtk::Video` as live thumbnail, click toggles
   play/pause via `MediaFile::get_playing()`/`play()`/`pause()`, "open in player"
   button launches the cached copy externally, media-error fallback to a plain chip.
2. **v2 presence** (roster rows): fleet heartbeat states are
   `down/starting/ready/acting/restarting` …â€”… the old code treated v1 daedalus states
   (`idle/done/...`) as not-busy, so `ready` showed a permanent spinner. Now: spinner
   only for acting/starting/restarting, colored state tag always (CSS classes
   `hv-state-ready` green / `hv-state-down` grey added to the inline CSS string ~line 100).

**Do**: run the E2E (`/tmp/hive_e2e.sh`, see below), update Plan.md's E bullet to DONE,
commit + push. The E bullet currently says "IN PROGRESS ... REMAINING: presence
indicators polish, video attachments" …â€”… both are now done.

## What v2 is

Hub-and-spoke: **hive-server** = pure comms hub (threads/roster/approvals/skills/
keys; NO daedalus, NO LLM keys, NO podman). **hive-worker** = per-rig daemon that owns
podman + LLM credentials; runs agent brains as `hive-agent` (daedalus::Agent in a
container, stdio NDJSON protocol) via `hwfleet::Fleet`. **hive-client** = GTK4 app.

Key protocol pieces: worker connects out to hub :8423 WS (`hello`+key, caps,
`models[]` catalog, heartbeat with per-agent states). Ops server->worker:
`agent_spawn/agent_stop/agent_remove/task/cancel/approval_resp`; worker->server:
`task_delta/task_result/approval_req/agent_status/agent_msg/...`.

## Phases done (all committed, Plan.md has per-phase notes)

- **A** hive-agent binary (`b97a1bf`): daedalus brain in the box.
- **B** worker Fleet (`2be9224`): container = env, brain = `podman exec -i hive-agent
  /workspace`, worker-held pipes, monitor thread with crash-respawn backoff.
- **C** hub routing (`d989ad8`): HubRouter + ChatService; deleted
  AgentHost/LLMStore/McpBridge/ModelRoutes/AgentRoutes + daedalus linkage from the
  server. Roster keeps model/provider as NAMES; workers resolve via
  `~/.local/share/hive-worker/models.json` + `secrets.json` (0600) …â€”… credentials
  never transit the hub.
- **D** worker hardening (`cc8ff1e`): mcp_rpc bridge dead code gone, skill sync =
  canonical push + `/skills:ro` mount (no symlinks), routes channel-only, new
  `agent_remove` op, ContainerManager cut to statics-only.
- **E core** (`758d50a`): worker hello carries models catalog -> GET /api/models;
  POST /api/agents/:id/start|stop -> hub; client New Agent dialog + inspector
  rewired; dead ApiClient methods deleted.

## Verification

- `cmake --build build -j$(nproc)` then `ctest --test-dir build` (15/15).
- `/tmp/hive_e2e.sh` …â€”… full live E2E: spins fresh server+worker (temp HOMEs,
  `XDG_DATA_HOME=$HOME/.local/share` so podman sees real images), enrolls, creates
  agent, real chat round-trip asserting `sender_kind=agent` "HUB ALIVE", then
  MODELS/STOP/START/RESPAWN/REMOVE/CONTAINER-GONE checks. Recreate it from git
  history if /tmp was wiped. It needs a valid `models.json`+`secrets.json` for the
  test worker (script writes them) and the `localhost/hive-agent:v2` image:
  `podman build -t localhost/hive-agent:v2 --build-arg HIVE_UID=$(id -u)  hive-server/data/Containerfile.hive-agent .`

## Hard rules (from memory, don't relearn the hard way)

- **Never patch OmniNet or daedalus** …â€”… bugs are in our code; use their public APIs.
- Python heredoc edits must be **ASCII-only** …â€”… em-dashes/arrows mangle to 0x85 in
  transport and break the script. Match with ASCII anchors + line-index edits.
- `pkill  "build/hive..."` kills your own shell …â€”… use `pkill -x <name>`.
- OmniNet `pathParam(i)` is unreliable for multi-param routes …â€”… routes parse
  `req.path()` via `path_segments()`.
- OmniNet WS: use onText/onBinary + readLoop (never per-call receiveText), set
  `keepAliveTimeout=0` for WS channels, HTTP+WS servers each need their own thread.
- Rootless podman image storage is HOME-relative; fleet `rm` needs `-t 0` (container
  PID 1 is `sleep infinity`, ignores SIGTERM = 10s stall).
- message_agent is async-only by design (pair threads, generation-capped fan-in) …â€”…
  validated by the user, do NOT add sync wait/reply-relay.
- daedalus history saves ONLY via Agent::shutdown() …â€”… graceful stops matter.

## What's next

- **Commit the pending E-remainder** (above).
- **Phase F …â€”… droplet cutover**: deploy server + co-hosted worker on the droplet,
  migrate roster (`worker="droplet"`), history preserved (threads DB untouched).
  I have NO droplet SSH details in-repo or in memory (old `dev-rig` host is retired;
  `~/.ssh/config` empty). Needs the user's credentials/hostname and ideally them
  watching …â€”… it's production. `docs/deploy.md` has the topology/systemd layout.
- Plan.md section 8 has the full phase list; sections 9 (non-goals) and the docs/
  folder (`design.md`, `hive-worker.md`, `deploy.md`, `containers.md`, `api.md`)
  are the reference set to read first.

## Read these files first

1. `Plan.md` (phase notes A-F are dense and current)
2. `docs/design.md`, `docs/hive-worker.md`, `docs/deploy.md`
3. `hive-server/include/hivemind/HubRouter.h` + `ChatService.h` (hub core)
4. `hive-worker/src/fleet.h` (worker fleet core)
5. `hive-agent/src/main.cpp` (brain: stdio protocol, message_agent tool)
6. `hive-server/src/WorkerChannel.{h,cpp}` (channel protocol ground truth)
