# ⚡ HiveMind Node Graph

A visual **multi-agent orchestration system**. Build workflows out of small,
single-purpose nodes on a canvas, wire them together, and click **Run** — the
server walks the graph and executes each node, including full AI agents with
tool use, MCP servers, and skills.

![MIT license](https://img.shields.io/badge/license-MIT-blue)
![C++23](https://img.shields.io/badge/C%2B%2B-23-blue)

---

## What it does

- **Canvas editor** — drag nodes, wire ports, pan/zoom, auto-layout. No frontend
  build step; the UI is a single vanilla-JS page.
- **8 node types** — `input`, `prompt`, `agent`, `merge`, `loop_start`,
  `loop_end`, `output`, `log`.
- **Real AI agents** — the `agent` node embeds
  [daedalus](https://gitlab.com/leestripp/daedalus): LLM streaming, tool calls,
  MCP, skills, context compression. Each agent node is its own isolated agent;
  fan-out → merge → fan-out multi-agent graphs run in parallel.
- **Loops** — bounded iteration with body-text feedback (`loop_start`/`loop_end`).
- **Workflow management** — save to SQLite, switch between workflows, New /
  Export / Import portable `.hivemind.json` files.
- **Live runs** — click Run and poll the event/output stream per node.

## Quick start

```bash
# 1. System packages (Ubuntu)
sudo apt install -y cmake g++ libcurl4-gnutls-dev nlohmann-json3-dev \
    sqlite3 libsqlite3-dev libssl-dev libpng-dev libpoppler-cpp-dev

# 2. Build & run
./start.sh --build          # configure + build + start on :8421
# or:
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
./build/hivemind --port 8421

# 3. Open the UI
xdg-open http://localhost:8421/
```

CMake fetches [daedalus](https://gitlab.com/leestripp/daedalus) (agent engine,
tag `0.9.6`) and [OmniNet](https://gitlab.com/leestripp/omninet) (HTTP server)
automatically on first configure.

### Models

`agent` nodes need a model configured in the daedalus **SettingsStore**
(`~/.local/share/daedalus/daedalus.db`) — the same store the
[icarus](https://gitlab.com/leestripp/icarus) CLI manages. HiveMind reads it
for `/api/models` and passes `config.model` straight to daedalus; it never
stores credentials itself. Once a model exists, pick it in the Agent
inspector's **Provider/Model** dropdown.

## Using the canvas

| Action | How |
|--------|-----|
| Add a node | Right-click canvas → **Add node** |
| Wire nodes | Drag from an output port to an input port (ESC cancels) |
| Move a node | Left-drag |
| Delete node / wire | Select it → **Delete**, or right-click → Delete |
| Pan | Middle-drag, or hold **Space** + drag |
| Zoom | Scroll wheel |
| Toggle panels | `]` inspector, `\` output stream |
| Configure a node | Click it → Inspector (model picker, project **Browse…**, tools/skills/MCP, etc.) |
| Run | **▶ Run** in the toolbar; output streams to the bottom panel |
| Workflows | Toolbar: switcher dropdown, **New**, **Save**, **Export**, **Import** |

## HTTP API (selected)

```
GET  /api/health                     liveness
GET  /api/node-types                 registered node types + port shapes
GET  /api/models                     models from the daedalus SettingsStore
GET  /api/agent/tools|skills|mcp     agent capability introspection
GET  /api/graphs                     list workflows
POST /api/graphs                     create workflow
GET/PUT/DELETE /api/graphs/:id       read / update / delete
POST /api/graphs/:gid/nodes|edges    add nodes / wires
POST /api/graphs/:gid/run            start a run → { run_id }
GET  /api/runs/:rid/output           poll the run's event/output stream
GET  /api/graphs/:id/export          download .hivemind.json
POST /api/graphs/import              upload .hivemind.json
GET  /api/fs/browse?path=…           server-side directory listing (project picker)
```

Full route table: `docs/design.md` §5.

## Tests

```bash
ctest --test-dir build --output-on-failure   # 3 suites: loop engine, node ports, daedalus smoke
```

## Project layout

```
src/            C++ server (main, GraphRunner, GraphStore, executors/)
include/hivemind/   public headers + routes/ (one file per API group)
web/dist/       the canvas UI (index.html, app.js, styles.css — served raw)
tests/          CTest suites
docs/design.md  architecture deep-dive
AGENTS.md       contributor/agent quick-start
NEXT.md         current work queue
```

## License

MIT — © Lee A. Stripp. See `LICENSE`.
