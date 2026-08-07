# HiveMind — Next Steps / Working Notes

> Status: 2026-08-06 — `llm` node removed (never wanted; the `agent` node covers
> all LLM work via daedalus). All remaining node types are implemented and
> unit-tested. Next milestone: GraphRunner end-to-end CTest, then a live
> `agent` node run.

## Quick project state

- **Build:** `build/hivemind` compiles cleanly.
- **Tests:** `ctest --test-dir build` → 3/3 pass (LoopEngine, node ports, daedalus smoke).
- **Server:** not running; start with `./build/hivemind --port 8421`.
- **Git:** clean working tree on `main`.

## Node-type status

| Node type | Backend | Status | Notes |
|-----------|---------|--------|-------|
| `input` | `InputExecutor` | ✅ | tested end-to-end; supports `param_name` + fallback `value`/`default_value` |
| `prompt` | `PromptExecutor` | ✅ | tested end-to-end |
| `output` | `OutputExecutor` | ✅ | keys results by `config.output_name` |
| `log` | `LogExecutor` | ✅ | tested end-to-end |
| `merge` | `MergeExecutor` | ✅ | `concat` strategy; `interleave` is future |
| `loop_start`/`loop_end` | `LoopEngine` | ✅ | unit + end-to-end tested with feedback/loop_count |
| `agent` | `daedalus::Agent` | ⏸️ | final milestone; needs a live provider |

There is intentionally **no `llm` node**. HiveMind runs agents; one-shot
LLM calls are just agents without tool use. daedalus already provides the
full agent loop, so a separate plain-LLM node would be redundant.

## What to do next

1. **Add a GraphRunner end-to-end CTest** (no API key needed)
   - Construct a graph in memory and call `runner.run_sync()` without a server.
   - Covers: `input → prompt → merge → output` and a separate loop graph.
   - Catches regressions in the runner's parallel dispatch and loop logic.

2. **Live `agent` node run** (needs an API key)
   - The daedalus SettingsStore (`~/.local/share/daedalus/daedalus.db`)
     already has `minimax` and `moonshot` configured with keys
     (see `GET /api/models`).
   - Run the sample multi-agent DAG from the UI
     (or a single `prompt → agent → output` graph via curl).
   - Verify `node_output` streaming events and the final result.

## What *not* to do yet

- Don't touch the UI beyond the node chrome already built.
- Don't add WebSocket support — v1 is HTTP polling (`GET /api/runs/:rid/output`).
- Don't add more node types (`if`, `filter`, `map`, etc.) until the current set is solid.

## How to pick up

```bash
cd /workspace/AI/Hive-Mind
cmake --build build -j$(nproc) && ctest --test-dir build --output-on-failure
./build/hivemind --port 8421
```


## Agent node Inspector — the big one (LAST)

The Agent editor is not a few text fields — it's a multi-section panel
with selectors fed by live backend data. `FIELD_SCHEMAS` already supports
`text / textarea / number / select / display`; the Agent work adds
**async-populated selects** and **multi-select checkbox groups**, plus a
`model picker` that groups by provider.

### Planned layout

Flat fields always visible; the three on/off groups live in
**collapsible `<details>` sections** (same pattern as Raw JSON) so the
panel stays compact. Summary line shows "N of M enabled".

```
Name            [ Greeter                    ]
Provider/Model  [ minimax — MiniMax-M3     ▼ ]   grouped by provider, from GET /api/models
Project root    [ /path/to/project           ]   working dir for tools
System prompt   [ (optional override)        ]   textarea
Auto-approve    [x] run shell commands without asking

▸ Tools        25 of 27 enabled          (GET /api/agent/tools ✅)
   [x] execute_bash  [x] read_file  [ ] create_file …
▸ Skills        2 of 3 enabled           (GET /api/agent/skills — NEW)
   [x] code-review  [ ] git-workflow …
▸ MCP servers   1 of 2 enabled           (GET /api/agent/mcp — NEW)
   [x] filesystem  [ ] github …
```

### daedalus on/off model (verified in AgentConfig.h)

Default is everything ON; the config carries **disable-lists**:

| daedalus `AgentConfig` field | Effect | Agent node config key |
|------------------------------|--------|-----------------------|
| `tools_disabled` (set<string>) | Hidden from LLM; call rejected | `tools_disabled: []` |
| `skills_disabled` (set<string>) | Hidden from discovery | `skills_disabled: []` |
| `mcp_servers_disabled` (set<string>) | Server skipped at startup | `mcp_servers_disabled: []` |
| `mcp_enabled` (bool, master) | All MCP off | `mcp_enabled: true` |

UI semantics: all checkboxes start **checked**; unchecking appends the
name to the corresponding `*_disabled` array. `AgentExecutor` copies the
arrays into `AgentConfig` verbatim.

### Backend work required

| Endpoint | Status | Backed by |
|----------|--------|-----------|
| `GET /api/models` | ✅ live (2 models: minimax, moonshot) | `SettingsStore::list_models/resolve_model` |
| `GET /api/agent/tools` | ✅ live (27 tools) | `Agent::list_tools()` |
| `GET /api/agent/skills` | ❌ add | `Agent::list_skills()` exists in daedalus |
| `GET /api/agent/mcp` | ❌ add | `Agent::mcp_configured_servers()` exists in daedalus |

Both missing endpoints are thin wrappers in `AgentRoutes.cpp` using the
existing `introspection_agent_config()` pattern.

### Frontend work required

1. Extend `FIELD_SCHEMAS` with `kind: 'multiselect'` (checkbox group) and
   async select population (`source: '/api/models'` fetched on inspector open).
2. Agent config shape (what `AgentExecutor` must learn to read):
   `model`, `project`, `system_prompt`, `auto_approve`, `mcp_enabled`,
   `tools_disabled[]`, `skills_disabled[]`, `mcp_servers_disabled[]`.
3. Executor support: map the three `*_disabled[]` arrays straight into
   `AgentConfig::tools_disabled / skills_disabled / mcp_servers_disabled`
   (verified — see table above). Trivial copy, no daedalus changes.
4. Keep raw JSON `<details>` as fallback throughout.

### Sequencing (when we start)

1. Backend: add `/api/agent/skills` + `/api/agent/mcp` (small).
2. ~~Verify daedalus-side allow-list mechanism~~ ✅ Done — disable-lists (`*_disabled` sets).
3. Model/provider dropdown slice (highest value, endpoints ready).
4. Project/system-prompt/auto-approve slice (plain fields, easy).
5. Tools/skills/MCP multi-selects (needs #2).


## Workflow save / load / files (BEFORE the Agent inspector)

Two persistence layers, distinct jobs:

| Layer | What | Status |
|-------|------|--------|
| **SQLite** (`GraphStore`) | Live working state — the server's source of truth | ✅ exists; UI already saves nodes+edges via `/api/graphs` CRUD at run time |
| **Workflow files** (JSON) | Portable export/import — share, version-control, back up, switch between workflows | ❌ to build |

### File format (v1)

One self-contained JSON document per workflow:

```jsonc
{
  "version": 1,                 // for future migrations
  "name": "expert-panel",
  "description": "...",
  "saved_at": "2026-08-07T00:00:00Z",
  "nodes": [ { "id", "type", "label", "config": {...}, "x", "y" } ],
  "edges": [ { "source", "source_port", "target", "target_port" } ]
}
```

Suggested extension: `.hivemind.json`. No DB-local foreign keys; node ids
are preserved in-file but **remapped to fresh UUIDs on import** to avoid
collisions (edge references rewritten to match).

### API additions

| Endpoint | Purpose |
|----------|---------|
| `GET  /api/graphs/:id/export` | Serialize graph → download `.hivemind.json` |
| `POST /api/graphs/import`     | Upload file → validate `version`, remap ids, create new graph (name + " (imported)") |
| `GET  /api/workflows`         | (later) list a server-side library dir `~/.local/share/hivemind/workflows/` |

### Frontend

1. **Save button** — explicit persist (today save only happens implicitly at Run).
2. **Export button** — `GET /api/graphs/:id/export` → browser download.
3. **Import button** — file picker → `POST /api/graphs/import` → open the new graph.
4. **Workflow switcher** — dropdown of `GET /api/graphs` in the top bar to
   change between saved workflows (replaces always loading the sample graph).

### Validation & edge cases

- Reject files without `version`, unknown `version`, missing `nodes`/`edges`.
- Reject unknown node `type`s (or import them disabled with a warning).
- Import never overwrites an existing graph — always creates a new one.
- Export is deterministic (sorted keys) so files diff cleanly in git.

### Sequencing

1. Backend export endpoint (pure serialization — easiest).
2. Backend import endpoint (validate + id remap).
3. UI Save/Export/Import buttons + switcher.
4. (Later) server-side workflow library dir + `/api/workflows` listing.


## Resizable & collapsible panels (UI) — ✅ DONE (aad0abe)

Real runs produce a lot of output — the output area needs to grow, and
panels need to get out of the way.

### Delivered

- Grid refactor: `.app` = 4 rows, `.main` = 3 cols; sizes via `--insp-w` /
  `--out-h` custom properties.
- 6px splitters with pointer-capture drag; focusable (`role="separator"`),
  arrows = 16px step, Shift+arrows = 48px.
- Collapse via head toggle button, splitter double-click, or collapsed
  strip click. Global keys: `]` inspector, `\\` output (ignored while
  typing in form fields).
- Persistence: `localStorage` key `hm.layout`
  (`{inspW, outH, inspCollapsed, outCollapsed}`), restored on load,
  clamped on window resize.
- Inspector collapsed strip: 28px vertical label; output collapsed: 28px
  title bar (the "latest status line" strip idea was dropped — the title
  bar is enough for v1).

### Original scope (kept for reference)

| Panel | Resize | Hide/show |
|-------|--------|-----------|
| Inspector (right) | Drag left edge — width 240–600px | Toggle button in top bar (and `]` key) |
| Output (bottom) | Drag top edge — height 80px–70% of viewport | Toggle button (and `\` key) |
| Canvas (center) | Fills remaining space automatically | Always visible |

### Implementation notes

1. **Splitters**: 4–6px drag handles between panels (`pointerdown/move/up`,
   `touch-action: none`). Change CSS grid tracks (`grid-template-columns`
   for inspector, `grid-template-rows` for output) during drag; live
   resize, no ghost line needed.
2. **Collapse**: animated (150ms) to zero track size; panel content kept
   in DOM so state (scroll position, inspector selection) survives.
   Collapsed output shows a thin strip with the latest status line +
   expand chevron.
3. **Persist layout in `localStorage`** (`hivemind.layout.v1`):
   `{ inspectorWidth, outputHeight, inspectorHidden, outputHidden }` —
   restore on load. Client-side only; nothing in the backend.
4. **Accessibility**: splitters focusable, arrow-key resize (10px step),
   `role="separator"` with `aria-orientation`.
5. Canvas pan/zoom must re-clamp after resize (viewport size changed).

### Sequencing

1. CSS grid refactor + splitters (inspector width, output height).
2. Hide/show toggles + thin collapsed strip for output.
3. `localStorage` persistence + keyboard support.
