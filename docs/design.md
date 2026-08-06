# HiveMind Node Graph — Design

> **Audience:** contributors, reviewers, and AI coding agents picking up the project.
> **Goal:** explain *what HiveMind is*, *what problem it solves*, *how the
> pieces fit together*, and *how to extend it* — in one place.

---

## Contents

1. [Overview](#1-overview)
2. [Core concepts](#2-core-concepts)
3. [Node types](#3-node-types)
4. [Wiring & execution model](#4-wiring--execution-model)
5. [HTTP server (OmniNet)](#5-http-server-omninet)
6. [AI Agent engine (daedalus)](#6-ai-agent-engine-daedalus)
7. [Persistence (SQLite)](#7-persistence-sqlite)
8. [Worked examples](#8-worked-examples)
9. [Extension points](#9-extension-points)
10. [Open questions / future direction](#10-open-questions--future-direction)

---

## 1. Overview

**HiveMind Node Graph** is a visual workflow editor built around
directed graphs of small, single-purpose **nodes**. The user places
nodes on a canvas, **wires** them together with edges that carry text,
and clicks **Run**. Some of those nodes are **AI Agents** — they
reason, call tools, search the web, and emit text — but every node is
a *peer*. AI Agents do not run the graph and they do not walk it. The
graph runs itself.

Concretely: the server's **runner** (a piece of orchestration code —
not an AI Agent) walks the graph in topological order, and the moment a
node's inputs are all resolved it dispatches that node's executor. The
`agent` node fires on input-ready the same way `prompt`, `log`, and
`merge` do. The runner only knows about node-type dispatch; it has no
opinion on what an AI Agent is or does.

### Why a graph?

A graph is the smallest model that captures what agentic workflows actually
need:

- **Sequencing** — "the user prompt feeds the agent, the agent's output
  feeds the next step". This is `A → B → C` edges.
- **Branching / merging** — "two agents each look at the same prompt, then
  we merge their outputs into a summary". This is one source feeding two
  edges, then a Merge node with two inputs feeding one edge.
- **Looping** — "the agent reviews its own answer and revises it up to
  three times". This is a cycle, made explicit and bounded by dedicated
  Loop nodes.
- **Parallelism** — any node whose inputs are all ready can run at the same
  time as its peers; the engine exploits that for free.

Linear "one-prompt, one-response" interfaces (chat boxes, CLI REPLs) hide
this complexity. HiveMind exposes it on a canvas so users can see and
control it.

### What it is, in one paragraph

A C++23 server that:

- exposes a REST API (powered by **OmniNet**) for graph CRUD and runs,
  plus a live, pollable event stream for output;
- persists graphs and run history in SQLite;
- executes each node by dispatching to a type-specific **executor**
  (LLM calls, text I/O, loops, logs, etc.);
- uses **daedalus** as the backing library for the `agent` node type
  (the other node types back onto libcurl, SQLite, or `std::cerr`);
- serves a vanilla-JS canvas UI from `web/dist/` so the user can build and
  run graphs in a browser.

### What it is *not*

- **Not a chat product.** There is no conversation history, no chat UI.
  The canvas is the product.
- **Not a multi-tenant SaaS.** It's a single-process local server. Auth,
  RBAC, quotas are out of scope.
- **Not an agent framework.** HiveMind composes AI-agent invocations; it
  does not implement tool use, MCP, skills, or context compression itself.
  All of that lives in **daedalus**.
- **Not a workflow scheduler.** Graphs run when the user clicks **Run**.
  No cron, no triggers.

---

## 2. Core concepts

### 2.1 Node

A **node** is a single unit of work. Each node has:

| Field | Type | Description |
|-------|------|-------------|
| `id` | UUID | Stable across saves. |
| `type` | string | One of the registered node types (see §3). |
| `label` | string | User-facing name shown on the canvas. |
| `config` | JSON | Type-specific configuration (see §3). |
| `position` | `{x, y}` | Canvas coordinates (purely cosmetic). |
| `inputs` | `vector<Port>` | Derived from `type`, **not** stored. |
| `outputs` | `vector<Port>` | Derived from `type`, **not** stored. |

A node's **ports** are not stored — they are derived at runtime from the
node's `type` via `Node::ports_for_type(type)`. This guarantees that
"an `agent` node always has `in` and `out`" is a property of the type, not the
node instance, so the canvas never disagrees with the executor.

### 2.2 Port

A **port** is a named, typed connector on a node.

```cpp
struct Port {
    std::string name;       // e.g. "in", "out", "branch_a"
    std::string type;       // "string" today; "json" reserved
    std::string direction;  // "input" | "output"
};
```

Today every port carries `type = "string"`. The `json` variant is reserved
for future use (e.g. structured tool-call payloads). All edges transmit
strings; the executor is responsible for any deserialisation.

### 2.3 Edge

An **edge** connects an output port of a source node to an input port of
a target node. Edges are stored.

```cpp
struct Edge {
    std::string id;
    std::string graph_id;
    std::string source_node;
    std::string source_port;
    std::string target_node;
    std::string target_port;
};
```

Edges carry no data themselves — they are pure wiring. The value that
flows along an edge at runtime lives in the executor's `resolved_` map
keyed by `"<node_id>:<port>"`.

### 2.4 Graph

A **graph** is a collection of nodes + edges, plus metadata. Stored as a
header row (`graphs`) and child rows (`nodes`, `edges`).

### 2.5 Run

A **run** is a single execution of a graph. Each run gets a UUID
(`run_id`) and an independent executor. Two concurrent runs of the same
graph do not share state.

A run has a lifecycle:

```
running  ──▶  completed   (all nodes finished, no errors)
running  ──▶  failed      (an executor threw; the run stops)
running  ──▶  cancelled   (the user clicked Stop; the run cooperatively cancels)
```

---

## 3. Node types

All node types are registered in `src/ExecutorRegistry.cpp` (the single
source of truth for "type string → executor instance"). The canvas
discovers the set of available types by hitting `GET /api/node-types`.

### 3.1 Built-in types (v1)

| Type | Inputs | Outputs | Backed by | What it does |
|------|--------|---------|-----------|--------------|
| `prompt`    | — | `out`           | static text | Returns its `config.value` as a single string. |
| `agent`     | `in` | `out`        | **`daedalus::Agent`** | Delegates text-in / text-out to an embedded `daedalus::Agent` with tool calls, MCP, skills, streaming. Each `agent` node runs as its own independent `daedalus::Agent` instance; multiple `agent` nodes in the same graph run as parallel peers. |
| `input`     | — | `out`           | runner input map | Reads a named key from the `POST /api/graphs/:id/run` payload. |
| `output`    | `in` | —            | run result map | Collects a named value into the run's return payload. |
| `loop_start`| `in` | `out`, `loop_count` | `LoopEngine` | Loop entry: passes `in` to `out` and emits `loop_count` to the paired `loop_end`. On iteration > 1, `in` is the body text feedback from the previous `loop_end`. |
| `loop_end`  | `in`, `loop_count` | `out`        | `LoopEngine` | Loop exit: receives body text and loop count. If `current < target`, the runner feeds the body text back to `loop_start`; otherwise emits `out` and completes the loop. |
| `merge`     | `in_a`, `in_b` (extendable) | `out` | string concatenation (default) | Combines multiple inputs into one. v1 ships concat; future revisions add join strategies. |
| `log`       | `in` | —            | `std::cerr` | Prints the input to the server log. No output. |

### 3.2 Per-type config schemas

```jsonc
// prompt node
{ "value": "Hello, write me a README." }

// agent node (daedalus)
{
  "model":        "minimax",                        // model name in the daedalus SettingsStore
  "project":      "/path/to/project",               // agent's working directory (tools use this)
  "auto_approve": true,                             // auto-approve execute_bash by default
  "system_prompt": "..."                            // optional override of composed prompt
}

// input node
{ "param_name": "target_dir", "param_type": "string" }

// output node
{ "output_name": "readme_content", "output_type": "string" }

// loop_start
{
  "loop_count": 3   // number of iterations; copied to the paired loop_end
}

// loop_end
{
  "loop_count": 3   // authoritative target iteration count
}

// merge
{
  "strategy": "concat",     // "concat" (default) | "interleave"
  "separator": "\n\n"       // for concat
}

// log
{}
```

### 3.3 Multi-agent graphs

Most useful HiveMind graphs have **more than one `agent` node**. The
pattern that shows up most often is *fan-out → merge → fan-out*:

```
   prompt               ┌──> Agent A ──┐
     │                  │              │
     └─── shared ───────┼──> Agent B ──┤
                        │              ├── merge ──> Agent C ──> output
     ┌─ different ──────┼──> Agent D ──┘
     │                  │
     ▼                  │
   prompt               │
```

The graph runner orchestrates all of this — Agents A, B, and D run in
parallel (bounded by `max_concurrent`); the `merge` node waits for all
three; Agent C reads the merged text as its prompt. The agents
themselves are unaware of the graph; they just receive input on `in`
and emit text on `out`.

The implications of multiple `agent` nodes in one graph:

- **Independent state per agent.** Each `agent` node is its own
  `daedalus::Agent` instance with its own conversation history, tool
  state, MCP connections, and session directory. Agents do not share
  history by default — the only channel between them is the edges.
- **Edges are the only communication channel.** Cross-agent data flow
  is text on edges, optionally via a `merge` node. There is no other
  way for agents to share state.
- **Parallel streaming.** Each agent's tokens stream independently
  over the live event stream. The UI tags every `node_output` event
  with its `node_id` and renders one scroll region per node.
- **Concurrency hazards.** Two agents with the same `project` racing
  on the filesystem is the most common failure mode — see §6.6.

### 3.4 Merge node semantics

A `merge` node has multiple named input ports (`in_a`, `in_b`, …) and
one output port (`out`). v1 ships one join strategy; future revisions
expose more.

| Strategy | Behaviour | When to use |
|----------|-----------|-------------|
| `concat` (default) | `"<in_a><separator><in_b>"` with `separator = "\n\n"` | General-purpose. The downstream `agent` receives the joined text as a single prompt. |
| `interleave` (v2) | round-robin per-line or per-character | Rare. Useful when the join needs structural preservation (e.g. alternating turns of a debate). |

For multi-agent DAGs, `concat` is almost always correct: the
downstream agent receives the merged text and decides what to do with
it.

---

## 4. Wiring & execution model

### 4.1 Validation

Before a run starts, the engine validates the graph:

1. **Cycle detection** (Kahn's algorithm). Cycles outside `loop_start → … →
   loop_end` pairs are rejected. The engine does not try to be clever
   about implicit cycles.
2. **Port-type check** (v2 — currently best-effort). Every edge connects
   matching types. Today both ends are `string` so this is a no-op.
3. **Required config fields**. Each executor validates its own config at
   construction time and throws `std::invalid_argument` if anything is
   missing.

### 4.2 Execution loop

The runner is the **conductor**. It does not know what any individual
node does — it just detects when a node's inputs are all resolved and
dispatches that node's executor. Every node is treated identically:
same ready-queue, same parallelism rules, same cancellation semantics.
The fact that one of those node types delegates to a `daedalus::Agent`
is invisible to the runner; the `agent` node fires on input-ready the
same way `prompt`, `log`, and `merge` do.

```
1.  Find all nodes with no incoming edges. These are the start nodes.
2.  Mark each start node as "ready" and put it in the ready queue.
3.  Loop:
    a.   Drain the ready queue, dispatching each node on a worker thread
         (sequential for loop nodes, parallel for everything else — in
         particular, multiple `agent` nodes fan out in parallel when
         they share an upstream source).
    b.   When a node finishes:
         -   collect its outputs into the `resolved_` map
         -   for every edge out of the node: check if the downstream
             node's inputs are all ready; if so, add it to the ready
             queue
    c.   Special-case: when a `loop_end` finishes, consult `LoopEngine`.
         If `current < target`, the runner feeds the body text back to the
         matching `loop_start` as its `in` value and re-enqueues it (and the
         body nodes). If `current == target`, `loop_end` emits its `out` port.
    d.   If the ready queue is empty:
         -   if any `loop_start` was re-enqueued, continue
         -   else the run is complete
4.  Emit `graph_completed` or `graph_failed`.
```

Parallel dispatch is bounded by `max_concurrent` (default 4) to avoid
hammering the LLM provider and to keep tool-side side effects (file
writes, shell commands) from racing. The loop node pair is always
sequential because order matters.

### 4.3 Cancellation

The runner is cancellable via `std::stop_token`. Sources of cancellation:

| Source | Where | Effect |
|--------|-------|--------|
| User clicks **Stop** in the UI | `POST /api/runs/:rid/cancel` | `GraphRunner::cancel()` → atomic flag |
| SIGINT / SIGTERM | `main.cpp` signal handler | global shutdown flag → `server.stop()` |
| HTTP client disconnects | polling stops | run completes naturally (we don't kill runs mid-flight) |

Cancellation is cooperative: nodes that are mid-LLM-call check the token
between SSE chunks; nodes that are mid-`execute_bash` let the subprocess
finish unless the runner is destroyed.

### 4.4 The event stream

Every run emits a JSON event stream that the UI consumes by polling
`/api/runs/:rid/output` (WebSocket `/ws/runs` is planned). Every per-node
event carries the `node_id` so
clients can route it to the correct node's display region — this
matters in multi-agent graphs where multiple `node_output` streams
arrive concurrently. The event shapes are stable and form a public
contract:

| Event | Payload | When |
|-------|---------|------|
| `graph_started`    | `{graph_id, run_id}` | Run starts. |
| `node_started`     | `{node_id, node_label}` | An executor is about to run. |
| `node_output`      | `{node_id, chunk}` | A streamed token (LLM / agent only). |
| `node_tool_call`   | `{node_id, tool, args}` | Agent decided to call a tool. |
| `node_tool_result` | `{node_id, tool, result}` | Tool returned. |
| `node_state_change`| `{node_id, from, to}` | Agent state machine transition. |
| `node_compaction`  | `{node_id, before, after}` | Context was compressed. |
| `node_completed`   | `{node_id}` | Executor returned. |
| `node_error`       | `{node_id, kind, message, will_retry}` | Recoverable / fatal error. |
| `graph_completed`  | `{results: {...}}` | All nodes done. |
| `graph_failed`     | `{error}` | An executor threw. |
| `graph_cancelled`  | — | User cancelled. |
| `run_done`         | — | Always last; the event stream ends after this. |

The event stream is the integration contract for any UI, CLI, or
downstream consumer. New event types are additive; existing ones do not
change shape.

---

## 5. HTTP server (OmniNet)

The HTTP layer is **OmniNet** (`gitlab.com/leestripp/omninet`), fetched
via CMake `FetchContent` (or `-DOMNINET_LOCAL=…`). HiveMind uses
`OmniNet::HTTP::Server` directly; REST routes are registered with
`server.get(...)` / `server.post(...)` and the web UI is served from
`web/dist/` by a small custom file handler. We do **not** patch OmniNet
source code; if a public API does not behave as expected we find an
alternative public API rather than editing the dependency.

Port: `8421` (default, overridable with `--port` or `HIVEMIND_PORT`).

### 5.1 REST surface

| Method | Path | Purpose |
|--------|------|---------|
| `GET`    | `/api/health`              | Liveness probe. `{"status":"ok"}` |
| `GET`    | `/api/node-types`          | List registered executor types + port shapes. |
| `GET`    | `/api/models`              | List configured LLM models (`name, host, has_key, …`). |
| `GET`    | `/api/projects`            | List known project roots the agent can be pointed at. |
| `GET`    | `/api/agent/tools`         | List tools the embedded `daedalus::Agent` exposes. |
| `GET`    | `/api/agent/status`        | Agent state + stats for the current session. |
| `GET`    | `/api/graphs`              | List all graphs. |
| `POST`   | `/api/graphs`              | Create graph. Body `{name, description}`. |
| `GET`    | `/api/graphs/:id`          | Read full graph (nodes + edges). |
| `PUT`    | `/api/graphs/:id`          | Update graph metadata. |
| `DELETE` | `/api/graphs/:id`          | Delete graph + all nodes + edges. |
| `POST`   | `/api/graphs/:gid/nodes`   | Add node. |
| `PUT`    | `/api/graphs/:gid/nodes/:nid` | Update node config / position. |
| `DELETE` | `/api/graphs/:gid/nodes/:nid` | Delete node + connected edges. |
| `POST`   | `/api/graphs/:gid/edges`   | Add edge. |
| `DELETE` | `/api/graphs/:gid/edges/:eid` | Delete edge. |
| `POST`   | `/api/graphs/:gid/run`     | Start a run. Body `{inputs: {...}}`. Returns `202 + {run_id}`. |
| `GET`    | `/api/runs/:rid`           | Poll run status + accumulated results. |
| `POST`   | `/api/runs/:rid/cancel`    | Request cooperative cancellation. |
| `GET`    | `/api/runs/:rid/output`    | Poll the live event stream for a run (see §5.2). |
| `GET`    | `/`, `/*`                  | Static files from `web/dist/` (custom handler, not `OmniNet::HTTP::StaticHandler`). |

### 5.2 Live streaming (v1: HTTP polling; WebSocket planned)

v1 exposes the same event stream defined in §4.4 over HTTP polling:

- `POST /api/graphs/:gid/run` returns immediately with a `run_id` and
  an `output_url` (`/api/runs/:rid/output`).
- The UI polls `GET /api/runs/:rid/output` to receive the accumulated
  event log and `finished` flag.

The events are identical to the WebSocket events described in §4.4; only
the transport differs. WebSocket (`/ws/runs`) will be added in a later
revision using `OmniNet::HTTP::Server::registerProtocolSwitch`, which is
OmniNet's public API for upgrading HTTP connections to WebSocket without
patching the library.

---

## 6. AI Agent engine (daedalus)

The `agent` node delegates text-in / text-out to an embedded
[`daedalus::Agent`](https://gitlab.com/leestripp/daedalus) — the same
shape as every other node (text in, text out), but with the most capable
backing library underneath. Other node types back onto SQLite
(`input` / `output`) or `std::cerr` (`log`).
The `agent` node is **not** hierarchically special; it's just one of
several executors, and its executor happens to embed
[`daedalus`](https://gitlab.com/leestripp/daedalus) — a C++23 static
library (`libdaedalus.a`, target name `daedalus`) by `@leestripp` that
implements the full agent loop (LLM streaming, tool dispatch, MCP,
skills, context compression, crash-resilient session persistence).

### 6.1 Embedding contract

```
┌────────────────────────────────────────────────────────┐
│                daedalus::Agent                         │
│   cfg: AgentConfig, sink: shared_ptr<AgentSink>        │
│                                                        │
│   initialize()                                         │
│   submit_user_message(text) ─┐                         │
│   run_one_turn(stop_token)  ◀┘                         │
│   cancel() / shutdown()                                │
└────────────────────────┬───────────────────────────────┘
                         │ sink callbacks fire on agent threads
                         ▼
┌────────────────────────────────────────────────────────┐
│   AgentSink subclass owned by HiveMind                  │
│   on_assistant_delta(text)   →  emit `node_output`     │
│   on_tool_call(name, args)   →  emit `node_tool_call`  │
│   on_tool_result(name, res)  →  emit `node_tool_result`│
│   on_state_change(from, to)  →  emit `node_state_change`│
│   on_error(kind, msg, retry) →  emit `node_error`       │
│   on_compaction(b,a)         →  emit `node_compaction` │
│   on_log(level, msg)         →  std::cerr              │
│   request_command_approval() →  return auto_approve    │
└────────────────────────────────────────────────────────┘
```

### 6.2 Per-agent-instance model

Every time the runner dispatches an `agent` node, the executor
constructs a **fresh `daedalus::Agent`** for that execution. The
instance:

- owns its own conversation history;
- owns its own MCP connections (one transport per server, scoped to
  this instance);
- owns its own session directory under
  `<project_root>/.daedalus/<session_id>/`;
- holds its own prompt cache — there is no spillover between agents in
  the same graph, which is what you want: Agent C must not see Agents
  A and B's contexts unless you explicitly wire those edges;
- is destroyed (and `shutdown()` called) when the node finishes.

Concrete consequences for graph authors:

- **Two `agent` nodes in one graph are fully isolated from each other.**
  They share no memory, no history, no cache. The only way for them
  to communicate is via edges (string ports) — and optionally via a
  `merge` node.
- **Cancellation is per-instance.** Cancelling a run cancels every
  agent instance in that run independently; each instance responds to
  its own `stop_token`. A clean cancel does not leave partially-active
  daedalus state — each `Agent`'s destructor runs in RAII-clean order.
- **Long-running state across runs is opt-in (v2).** Today an agent's
  session is scoped to a single `run_id`. Cross-run continuity is
  tracked under §10 open questions.

The construction pattern in `AgentExecutor::execute()`:

```cpp
daedalus::AgentConfig cfg = /* per-node config + model lookup */;
auto sink = std::make_shared<GraphRunnerSink>(/*runner, node_id*/);
daedalus::Agent agent{std::move(cfg), std::move(sink)};
agent.initialize();
agent.submit_user_message(joined_inputs);
agent.run_one_turn();   // returns on stop / cancel / error
// dtor → agent.shutdown()
```

### 6.3 Why daedalus (not roll-our-own)

The `agent` node needs an LLM streaming client, a tool-call dispatch
loop, MCP integration, skill discovery, context compression, and
crash-resilient session persistence. Building all of that for one
node type would be the bulk of the project. daedalus is the upstream
implementation of exactly those concerns, versioned and supported by
its author. Embedding it lets HiveMind stay small (~3 kLOC) and
inherit features for free.

| Concern | Implementation in daedalus |
|---------|---------------------------|
| LLM streaming | production parser w/ retry, fallback, usage trailer |
| Tool calls | full OpenAI-compatible `tool_call` loop |
| MCP clients | stdio + HTTP transports |
| Skills | filesystem discovery + frontmatter |
| Context compression | 70/30 split + LLM summary |
| Session persistence | atomic writes + SIGSEGV handler |
| Provider plugins | `openai-compatible` / `minimax` / `moonshot` / `lmstudio` |

### 6.4 Auto-approval policy

`daedalus::AgentSink::request_command_approval(cmd)` is called every
time the agent wants to run `execute_bash`. HiveMind's default is to
**return `true` (auto-approve)**, matching the icarus CLI default and
the current HiveMind behaviour.

If you want approval prompts:

1. Override `request_command_approval` in a custom `AgentSink`
   subclass that knows how to ask the user (WebSocket round-trip,
   modal dialog, etc.).
2. Persist the choice in a per-node config field
   (`auto_approve: false`) so individual agents in a graph can be
   locked down.

### 6.5 Cancellation

`daedalus::Agent::cancel()` is idempotent and sets a thread-safe flag
that the agent loop checks between chunks / tool calls. HiveMind calls
`agent.cancel()` when the GraphRunner's `cancelled_` atomic flips.
Tools that block on a subprocess (e.g. `execute_bash`) check the token
between process polls; the long-running variant honours `stop_token`
natively.

Per-agent-instance cancellation means a cancelled graph does not
leave partially-active daedalus state — each agent's destructor runs
in RAII-clean order (§6.2).

### 6.6 Concurrency hazards in multi-agent graphs

Two agents in the same graph running in parallel can trip each other
up in ways that don't show up in single-agent testing. This is the
natural consequence of two independent stateful agents living in one
process — they are not bugs in daedalus.

| Hazard | Mitigation |
|--------|-----------|
| Two agents with the same `project` both call `execute_bash` → race on `git`, the working tree, file locks, package installs. | Distinct `project` paths per agent; or `max_concurrent = 1` for that run. |
| Two agents both call `create_file` on the same path → last writer wins, neither knows about the other. | Same as above, or serialise via `loop_start` → A → `loop_end` → B. |
| Two agents trigger expensive initialisation in the same MCP server. | Each agent owns its own MCP connections; transports are thread-safe **per agent** but daedalus does **not** share MCP state across agents. |
| Prompt-cache invalidation. | daedalus caches per-`AgentConfig`, so independent agents don't pollute each other's caches. Verify cache behaviour in your provider-plugin settings. |
| UI: multiple concurrent `node_output` streams. | The UI must tag each `node_output` event with its `node_id` (already in the event payload) and render one scroll region per node — see §4.4. |

The pattern that almost always avoids all of these is: **each agent
gets a distinct `project` directory** (e.g. a separate worktree), and
the merge node feeds downstream agents with text, never shared paths.

---

## 7. Persistence (SQLite)

Database file: `~/.local/share/hivemind/hivemind.db`. Created on first
access. Migrations are applied idempotently at startup.

### 7.1 Schema (v1)

```sql
CREATE TABLE graphs (
    id          TEXT PRIMARY KEY,   -- UUID v4
    name        TEXT NOT NULL,
    description TEXT,
    created_at  TEXT NOT NULL,      -- ISO-8601
    updated_at  TEXT NOT NULL
);

CREATE TABLE nodes (
    id          TEXT PRIMARY KEY,
    graph_id    TEXT NOT NULL REFERENCES graphs(id) ON DELETE CASCADE,
    type        TEXT NOT NULL,      -- executor type string
    label       TEXT NOT NULL,
    config_json TEXT NOT NULL,      -- JSON, type-specific
    position_x  REAL NOT NULL DEFAULT 0,
    position_y  REAL NOT NULL DEFAULT 0
);

CREATE TABLE edges (
    id           TEXT PRIMARY KEY,
    graph_id     TEXT NOT NULL REFERENCES graphs(id) ON DELETE CASCADE,
    source_node  TEXT NOT NULL REFERENCES nodes(id) ON DELETE CASCADE,
    source_port  TEXT NOT NULL,
    target_node  TEXT NOT NULL REFERENCES nodes(id) ON DELETE CASCADE,
    target_port  TEXT NOT NULL
);

CREATE TABLE runs (
    run_id      TEXT PRIMARY KEY,
    graph_id    TEXT NOT NULL REFERENCES graphs(id) ON DELETE CASCADE,
    status      TEXT NOT NULL,      -- running | completed | failed | cancelled
    error       TEXT,
    started_at  TEXT NOT NULL,
    finished_at TEXT
);

CREATE TABLE run_results (
    run_id      TEXT NOT NULL REFERENCES runs(run_id) ON DELETE CASCADE,
    node_id     TEXT NOT NULL,
    port        TEXT NOT NULL,      -- output port name
    value       TEXT NOT NULL,
    PRIMARY KEY (run_id, node_id, port)
);

CREATE TABLE run_completed (
    run_id      TEXT NOT NULL REFERENCES runs(run_id) ON DELETE CASCADE,
    node_id     TEXT NOT NULL,
    order_idx   INTEGER NOT NULL,   -- execution order
    PRIMARY KEY (run_id, node_id)
);
```

### 7.2 Connection model

SQLite connections are **not** shared across threads. Each public method
on `GraphStore` opens a fresh connection, does its work, and closes.
This is slow per-call but safe. The runtime hot path (the executor) does
not touch SQLite during execution; it persists only at run boundaries
(start, complete, fail). All in-flight state lives in
`GraphRunner::resolved_` (per-run, in-memory).

### 7.3 Settings

LLM model configurations live in daedalus' SQLite SettingsStore
(`~/.local/share/daedalus/daedalus.db`). HiveMind reads them via
`daedalus::SettingsStore` and never stores credentials itself.
(the file managed by the upstream codemax / icarus tools). This is a
read-only compatibility shim. In a future release we will read from
`daedalus`'s SQLite-backed `SettingsStore` (under
`~/.local/share/daedalus/`) and ship a one-shot JSON→SQLite migration.

---

## 8. Worked examples

The shortest path to understanding what HiveMind does is to look at
real graphs. Each example below is shown as JSON (the storage format)
and as an ASCII canvas (the UI format).

### 8.1 Hello world — one prompt, one agent, one output

```
┌──────────┐    ┌──────────┐    ┌──────────┐
│ prompt   │───▶│  agent   │───▶│  output  │
│ "Hello!" │    │          │    │  greet   │
└──────────┘    └──────────┘    └──────────┘
```

```jsonc
{
  "name": "hello",
  "nodes": [
    { "type": "prompt", "label": "Greeting",
      "config": { "value": "Say hello to the user in one sentence." } },
    { "type": "agent",  "label": "Greeter",
      "config": { "model": "minimax" } },
    { "type": "output", "label": "greet",
      "config": { "output_name": "greet" } }
  ],
  "edges": [
    { "source": "<prompt-id>", "source_port": "out",
      "target": "<agent-id>",  "target_port": "in" },
    { "source": "<agent-id>",  "source_port": "out",
      "target": "<output-id>", "target_port": "in" }
  ]
}
```

**Run with:**

```bash
curl -X POST http://localhost:8421/api/graphs/<gid>/run \
     -H 'Content-Type: application/json' -d '{}'
# → 202 { "run_id": "<rid>" }
```

### 8.2 Two parallel agents feeding a merge (the canonical pattern)

```
              ┌──────────┐
              │ prompt   │
              │ "Topic"  │
              └────┬─────┘
                   │
        ┌──────────┴──────────┐
        ▼                     ▼
┌──────────────┐      ┌──────────────┐
│ agent        │      │ agent        │
│ "Pros"       │      │ "Cons"       │
└──────┬───────┘      └──────┬───────┘
       │ in_a                 │ in_b
       └──────────┬───────────┘
                  ▼
            ┌──────────┐
            │  merge   │  strategy: concat
            └────┬─────┘
                 ▼
            ┌──────────┐
            │  output  │
            └──────────┘
```

The two `agent` nodes run **in parallel** because their only input
(`prompt:out`) is the same source. The `merge` node waits for both.

### 8.3 Three-agent expert panel (A + B → merge → C)

The same fan-out / merge / fan-out shape from §3.3, fully worked.
Two agents independently answer the user's prompt, a third agent
reads both answers and produces a synthesised response.

```
              ┌──────────────┐
              │ input        │  topic = "Should we use SQLite or Postgres?"
              └──────┬───────┘
                     │
        ┌────────────┴────────────┐
        ▼                         ▼
┌──────────────┐          ┌──────────────┐
│ agent        │          │ agent        │
│ "sqlite"     │          │ "postgres"   │
│ project:     │          │ project:     │
│  ~/work/sql  │          │  ~/work/pg   │
└──────┬───────┘          └──────┬───────┘
       │ out                    │ out
       │      merge.in_a         │ merge.in_b
       └──────────┬──────────────┘
                  ▼
            ┌──────────┐
            │  merge   │  strategy = "concat"
            └────┬─────┘
                 │ out
                 ▼
            ┌──────────────┐
            │ agent        │
            │ "synthesise" │  reads both answers, writes the verdict
            └──────┬───────┘
                   │ out
                   ▼
            ┌──────────────┐
            │ output       │  verdict
            └──────────────┘
```

Why this works:

- The two upstream agents have **distinct `project` paths**, so their
  `execute_bash` / `read_file` calls cannot race (§6.6).
- The runner dispatches them in parallel (they share the upstream
  `input:out`, so both become ready at the same time).
- The `merge` node joins their outputs with a blank line separator.
- The synthesiser agent receives a single prompt of the form:

  > "You are reviewing two expert opinions. Here is the SQLite
  > expert's view:
  >
  > <text>
  >
  > Here is the Postgres expert's view:
  >
  > <text>
  >
  > Produce a synthesised verdict."

This is exactly the pattern from §3.3, just one fan-out level deeper.

### 8.4 Self-critique loop
```
            ┌──────────┐
   ┌───────▶│  prompt  │  "Write a haiku about X"
   │        └────┬─────┘
   │             ▼
   │        ┌──────────┐
   │        │  agent   │  "Draft haiku"
   │        └────┬─────┘
   │             ▼
   │        ┌──────────┐
   │        │  agent   │  "Critique"
   │        └────┬─────┘
   │             ▼
   │        ┌──────────────┐
   │        │ loop_end     │  iteration_count = 3
   │        └──────┬───────┘
   │               │ (back-edge to loop_start)
   │
   └──  loop_start
```

Each loop iteration reuses the same agents with fresh inputs. The
`loop_end` node bumps the counter; after three iterations the loop
exits and the final draft flows to `output`.

### 8.5 Real-world: code review

```
┌──────────────┐
│ input        │  path = "/path/to/repo"
└──────┬───────┘
       ▼
┌──────────────┐
│ agent        │  project = "/path/to/repo"
│ "reviewer"   │  reads files via daedalus tools
└──────┬───────┘
       ▼
┌──────────────┐
│ output       │  review_md
└──────────────┘
```

The agent uses daedalus's `read_file`, `grep_search`, and `execute_bash`
tools to walk the repo. The user watches the live event stream and sees
the agent's tool calls live.

---

## 9. Extension points

### 9.1 Add a new node type

1. Declare its port shape in `src/Graph.cpp::Node::ports_for_type()`.
2. Subclass `NodeExecutor` in
   `include/hivemind/executors/<NewType>Executor.h` and implement
   `execute()` + `type_name()`.
3. Implement in `src/executors/<NewType>Executor.cpp`.
4. Register in `src/ExecutorRegistry.cpp` — one line in the `Impl()`
   constructor.
5. Add a row to the **Node types** table in §3.1.
6. Add a unit test in `tests/` using the `hivemind_test()` macro.
7. Update the Web UI's node palette (`web/dist/app.js`) to render the
   new type.

### 9.2 Add a new API route

1. Pick a route group (`Graph`, `Run`, `Model`, `Project`, `Agent`) or
   create a new one.
2. New group: add `include/hivemind/routes/<X>Routes.{h,cpp}`,
   declare `register_<x>_routes(server, …)`, call it from `src/main.cpp`.
3. Existing group: edit the existing `<X>Routes.cpp`.
4. Update the table in §5.1.

### 9.3 Add a new daedalus provider / model

`daedalus` ships with built-in provider plugins (`openai-compatible`,
`minimax`, `moonshot`, `lmstudio`) and exposes `ProviderRegistry` so
hosts can add their own. HiveMind doesn't need to change — operators
add the new provider via the model config file.

### 9.4 Hook the event stream

Any HTTP client can poll `/api/runs/:rid/output` and consume the
event stream defined in §4.4. No HiveMind changes are required; the event
shapes are the integration contract. WebSocket subscription will be added
later without changing the event shapes.

---

## 10. Open questions / future direction

These are intentionally not decided in this document. They are
recorded so future contributors can find the conversation.

1. **Long-lived `daedalus::Agent` instances.**
   Today: one Agent per `agent`-node execution. Simple, isolated, but
   no continuity across runs.
   Alternative: a per-session Agent with persistence across runs and
   loop iterations. Better cache behaviour, harder cancellation, more
   complex state management.
   *Recommendation:* keep per-execution for v1, revisit after v1 ships.

2. **Per-node approval UI.**
   `request_command_approval` is currently auto-approved. A future
   revision could surface a modal in the Web UI for nodes where
   `config.auto_approve = false`. Requires a synchronous UI round-trip
   in the middle of a turn.

3. **Settings store migration.**
   Done: model/provider settings live solely in daedalus's
   SQLite-backed `SettingsStore` (`~/.local/share/daedalus/`).

4. **Typed ports.**
   Today every port is `string`. `json` is reserved in the schema. Once
   agents start producing structured outputs (tool_call results,
   tables), typed ports become the natural way to enforce shape at the
   edge level.

5. **Graph versioning.**
   Currently every save overwrites. A diff/branch model (à la Git)
   would let users iterate on graphs without losing old versions.
   Out of scope for v1.

6. **Multi-graph runs.**
   Today a run executes one graph. A "subgraph" node that calls
   another graph by ID would let users compose larger workflows from
   smaller tested ones. Same execution engine, different entry point.

---

## Appendix A — File layout (current)

```
.
├── CMakeLists.txt              # Root build script. FetchContent for daedalus + OmniNet.
├── start.sh                    # Convenience: build / start / lint.
├── AGENTS.md                   # Agent quick-start reference.
├── Plan.md                     # (optional) migration plan; not currently in repo.
├── docs/
│   └── design.md               # This file.
│
├── include/hivemind/           # Public C++ headers.
│   ├── Graph.h                 # Node, Port, Edge, Graph, GraphRun.
│   ├── GraphStore.h            # SQLite persistence.
│   ├── NodeExecutor.h          # Abstract base for executors.
│   ├── ExecutorRegistry.h      # type-string → NodeExecutor*.
│   ├── LoopEngine.h            # Loop iteration state.
│   ├── GraphRunner.h           # Topological executor.
│   ├── Settings.h              # CLI args.
│   ├── executors/              # One .h per node type.
│   └── routes/                 # One .h + .cpp per route group.
│
├── src/                        # Implementation for the hivemind executable.
│   ├── main.cpp
│   ├── Graph.cpp               # Node::ports_for_type().
│   ├── GraphStore.cpp
│   ├── GraphRunner.cpp
│   ├── LoopEngine.cpp
│   ├── ExecutorRegistry.cpp
│   ├── Settings.cpp
│   └── executors/              # One .cpp per node type.
│
├── tests/                      # CTest unit tests.
│
└── web/dist/                   # Vanilla JS canvas UI.
    ├── index.html
    ├── app.js
    └── styles.css

build/_deps/
├── daedalus-src/               # Fetched by CMake. libdaedalus.a.
└── omninet-src/                # Fetched by CMake (ours + daedalus' copy).
```

---

## Appendix B — Glossary

| Term | Meaning |
|------|---------|
| **Node** | A unit of work on the canvas. Has a `type`, `config`, and ports. |
| **Edge** | A wire between an output port and an input port. |
| **Run** | One execution of a graph. Has a `run_id` and independent state. |
| **Executor** | The C++ class that runs a node (one per type). |
| **Agent** (C++) | A `daedalus::Agent` instance, constructed per `agent`-node execution. Independent per node: no shared state between agents in the same graph. |
| **Agent** (UI) | The label users see on the canvas for an `agent`-type node — same shape as every other node, just with daedalus underneath. |
| **AgentSink** | The `daedalus::AgentSink` interface; the only way daedalus talks to the host. |
| **LoopEngine** | HiveMind's per-run counter for loop iterations. |
| **OmniNet** | The HTTP server library we embed (WebSocket-capable via public API). |
| **daedalus** | The C++23 agent library we embed (`libdaedalus.a`). |
