# /workspace — the company shared drive (THIS worker)

Every agent container **on this worker** mounts this same folder at
`/workspace` (read-write). It is the company drive: agents collaborate
through files here, not memory. This file is the map — read it before
doing file work, and keep it current as folders gain purpose.

## Workspaces are per-worker — there is NO sync

Each worker machine (rig) has its OWN independent `/workspace`. Files
you write here are INVISIBLE to agents homed on other workers, and
their files are invisible to you. There is no replication, no shared
mount, no background sync — by design (rigs can hold huge datasets).

Cross-rig hand-offs go through the THREAD, not the filesystem: say
where the work lives and what it's called, attach small files to the
thread, or ask the user to copy large ones. Never tell a teammate on
another rig to "just read it from /workspace".

## Layout

| Path | What lives there |
|---|---|
| `departments/<dept>/` | Department-owned work. Work in YOUR department's folder unless the task is explicitly cross-department. |
| `projects/<name>/` | Cross-department initiatives. One folder per project, with its own `README.md` (goal, status, who contributes). |
| `inbox/` | The user drops requests/files here for agents to pick up. Check it when asked "what's in the inbox". |
| `attachments/<thread-id>/` | Chat uploads, managed by the server. Read them; never reorganize. |
| `scratch/` | Temporary working files. May be cleaned at any time — nothing here is kept. |
| `archive/` | Finished/inactive work. Move, don't delete. |
| `.hivemind/` | Server-owned (prompts, internal state) — agents don't touch. |

## Departments

- `departments/chief-of-staff/` — coordination, cross-dept tracking, ops
- `departments/design/` — visual design, UI, wireframes, brand
- `departments/engineering/` — software development, code, infra
- `departments/executive/` — CEO: decisions, strategy docs, board materials
- `departments/marketing/` — content, campaigns, assets
- `departments/research/` — knowledge gathering, market intel, fast lookups

Each department folder gets a `README.md` naming its agents and local
conventions — create it when the department is created, keep it current.

## House rules

1. **Start in your department folder.** Cross-department work gets a
   `projects/<name>/` folder.
2. **Every project folder gets a README.md the day it's created**: goal,
   current status, where things stand. It's how the next agent (or the
   user) picks up the work without asking.
3. **Markdown and plain text first** — the universal interface; anyone
   (agent or human) can read them with no tools.
4. **Hand-offs are files, not memory** — and cross-rig hand-offs are
   MESSAGES, not files (see "per-worker" above).
5. **Names: kebab-case, ISO dates** (`2026-08-24-launch-plan.md`).
6. **Never delete** outside `scratch/`. Move to `archive/` instead.
7. `/home/hive` is your private container home — tools, notes, drafts
   not ready to share. The moment it's for the team, it goes to
   `/workspace`.
