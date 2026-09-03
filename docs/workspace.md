# /workspace — the company shared drive

Every agent container mounts the worker's shared workspace at `/workspace`
(read-write). It is the company drive: agents collaborate through files
here, not memory. The **live map** is `/workspace/index.md` on each
worker — agents should read it first; this document is the versioned spec
that map is generated/maintained from.

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
4. **Hand-offs are files, not memory.** Leaving work for another
   department? Write it where they work, and say so in the thread.
5. **Names: kebab-case, ISO dates** (`2026-08-24-launch-plan.md`).
6. **Never delete** outside `scratch/`. Move to `archive/` instead.
7. `/home/hive` is the agent's private container home — tools, notes,
   drafts not ready to share. The moment it's for the team, it goes to
   `/workspace`.

## Related

- The **`hive-workspace` skill** (global skills library) distils this for
  on-demand agent discovery.
- Mount mechanics (state tree, /skills, home volumes): `docs/containers.md`.
