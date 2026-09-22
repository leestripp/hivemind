# /workspace — the company shared drive (per worker)

Every agent container **on a given worker** mounts that worker's shared
workspace at `/workspace` (read-write). It is the company drive: agents
collaborate through files here, not memory. The **live map** is
`/workspace/index.md` on each worker — agents should read it first; this
document is the versioned spec that map is generated/maintained from.

## Per-worker, never synced

Each worker machine owns an **independent** workspace
(`~/.local/share/hive-worker/workspace/` on that rig). There is **no
replication or sync between workers** — by design: a rig may hold very
large datasets, and file locality (artifacts stay on the machine that
produced them) is a core architecture principle. A file written on one
rig is invisible to agents homed on any other rig.

Consequences:

- **Same-rig collaboration is free** — agents on one worker share
  `/workspace` fully.
- **Cross-rig hand-offs go through the thread** (messages + chat
  attachments), not the filesystem. Agents must never assume a teammate
  on another worker can see their files.
- The hub keeps **no copy** of workspace content (dumb hub; the
  `/api/files` listing is relayed from the worker live).

On first startup the worker seeds the workspace layout below plus the
default `index.md` (`hwrunner::seed_workspace`, hive-worker/src/runner.cpp).
Seeding is idempotent and **never overwrites** an existing `index.md` —
agents maintain the live map thereafter. The in-repo skeleton
(`hive-server/data/workspace/index.md`) is the canonical text and seeds
the hub's own workspace root via `ServerConfig::seed_workspace`.

## Layout

| Path | What lives there |
|---|---|
| `departments/<dept>/` | Department-owned work. Agents work in THEIR department's folder unless the task is explicitly cross-department. |
| `projects/<name>/` | Cross-department initiatives. One folder per project, always with a `README.md` (goal, status, contributors). |
| `inbox/` | The user drops requests/files here for agents to pick up. |
| `attachments/<thread-id>/` | Chat uploads, managed by the server. Read them; never reorganize. |
| `scratch/` | Temporary working files — may be cleaned at any time. |
| `archive/` | Finished/inactive work. Move, don't delete. |
| `.hivemind/` | Server-owned (prompts, internal state) — agents don't touch. |

## Departments

| Folder | Scope |
|---|---|
| `chief-of-staff/` | Coordination, cross-dept tracking, ops |
| `design/` | Visual design, UI, wireframes, SVG assets, brand |
| `engineering/` | Software development, code, infra, tooling |
| `executive/` | CEO: decisions, strategy, board materials |
| `marketing/` | Content, campaigns, brand assets, social |
| `research/` | Knowledge gathering, market intel, fast-lookup docs |

Each department folder has a `README.md` naming its agents and local
conventions — create it when the department is created, keep it current.

## House rules

1. **Start in your department folder.** Cross-department work gets a
   `projects/<name>/` folder.
2. **Every project folder gets a README.md the day it's created**: goal,
   current status, where things stand. It's how the next agent (or the
   user) picks up the work without asking.
3. **Markdown and plain text first** — the universal interface; anyone
   (agent or human) can read them with no tools.
4. **Hand-offs are files, not memory — within a rig.** Leaving work for
   another department ON THIS WORKER? Write it where they work, and say
   so in the thread. Handing off to an agent on ANOTHER worker? The
   files must travel through the thread (attachments) or a user-mediated
   copy — the other rig cannot see this filesystem.
5. **Names: kebab-case, ISO dates** (`2026-08-24-launch-plan.md`).
6. **Never delete** outside `scratch/`. Move to `archive/` instead.
7. `/home/hive` is the agent's private container home — tools, notes,
   drafts not ready to share. The moment it's for the team, it goes to
   `/workspace`.

## Related

- The **`hive-workspace` skill** (global skills library) distils this for
  on-demand agent discovery.
- Mount mechanics (state tree, /skills, home volumes): `docs/containers.md`.
