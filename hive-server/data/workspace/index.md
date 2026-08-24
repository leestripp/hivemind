# /workspace — the company shared drive

Every agent sees this same folder at `/workspace` inside their container.
It is mirrored bulk storage (btrfs RAID1) on the server. This file is the
map — read it before doing file work.

## Layout

| Path | What lives there |
|---|---|
| `departments/<dept>/` | Department-owned work. Work in YOUR department's folder unless the task is explicitly cross-department. |
| `projects/<name>/` | Cross-department initiatives. One folder per project, with its own `README.md` (goal, status, who contributes). |
| `inbox/` | The user drops requests/files here for agents to pick up. Check it when asked "what's in the inbox". |
| `attachments/<thread-id>/` | Chat uploads, managed by the server. Read them; don't reorganize. |
| `scratch/` | Temporary working files. May be cleaned at any time — nothing here is kept. |
| `archive/` | Finished/inactive work. Move, don't delete. |

## Departments

- `departments/engineering/` — software development, code, infra
- `departments/marketing/` — content, campaigns, assets
- `departments/executive/` — CEO: decisions, strategy docs, board materials
- `departments/chief-of-staff/` — coordination, cross-dept tracking, ops

Each has a `README.md` naming its agents and local conventions.

## Rules of the house

1. **Start in your department folder.** Cross-department work gets a
   `projects/<name>/` folder.
2. **Every project folder gets a README.md** the day it's created: goal,
   current status, where things stand. Keep it current — it's how the next
   agent (or the user) picks up your work without asking you.
3. **Markdown and plain text first.** They're the universal interface here —
   anyone (agent or human) can read them with no tools.
4. **Hand-offs are files, not memory.** Leaving work for another department?
   Write it where they work, and say so in the thread.
5. **Names: kebab-case, ISO dates** (`2026-08-24-launch-plan.md`).
6. **Never delete** outside `scratch/`. Move to `archive/` instead.
7. `/home/hive` is your private container home — tools, notes, drafts you
   aren't ready to share. The moment it's for the team, it goes here.
