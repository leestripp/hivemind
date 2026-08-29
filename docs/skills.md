# Skills — the global library and the local split

> How HiveMind layers a managed global skill library on top of daedalus'
> built-in skill discovery, and why agent-learned local skills are
> deliberately *not* managed centrally.

---

## Installing from Git

`POST /api/skills/install {git_url, path?}` shallow-clones a repo, finds
SKILL.md (repo root or the `path` subdirectory), validates the frontmatter
name, and copies the skill into the global library. 409 on name conflict.
The web Skills page has an "Install from Git" button wrapping this.

## The two kinds of skills

| | **Global skills** | **Local skills** |
|---|---|---|
| Owned by | hive-server (the hive) | the individual agent + its user |
| Lives at | `~/.local/share/hivemind/skills/<name>/` | `<agent_dir>/.agents/skills/<name>/` |
| Managed via | Web Skills page, `/api/skills` | the agent itself (teach-by-demonstration), `/api/agents/:id/skills` |
| Audience | every agent that enables it | that one agent |

The split exists because of scale: with hundreds of projects and agents,
centrally managing every locally-learned skill would be unmanageable noise.
Global skills are the *curated, shared* capabilities; local skills are
personal working knowledge that belongs to the agent's own workspace.

## How agents discover skills (daedalus, unchanged)

daedalus' `SkillManager` scans three layers, in order:

1. `~/.local/share/daedalus/skills/` — daedalus user-global
2. `~/.agents/skills/` — Anthropic-standard user-global
3. `.agents/skills/` — project-local (for a roster agent: its agent dir)

HiveMind adds its global library by **symlinking** enabled skills into layer
3 — no daedalus patching, and one source of truth: edit a global skill once
and every enabled agent sees the update on its next turn.

## The enable mechanism: relative symlinks

Enabling a global skill for an agent creates:

```
<agent_dir>/.agents/skills/<name>  →  ../../../../skills/<name>
```

The target is **relative** (four levels up from the link's directory =
the hivemind data root) so the link keeps resolving when the data root is
bind-mounted into an agent container at the same relative depth.

> **Container caveat:** the container must bind-mount
> `~/.local/share/hivemind/skills/` at the matching depth, or the links
> dangle. This is a Phase 2+ container-layer requirement.

Enable rules enforced by the server:

- A global skill **cannot clobber** an agent-local skill with the same name
  (enable returns `409`). Local wins; rename one side.
- Deleting a skill from the global library **removes every agent's symlink
  first**, so no agent is left with a dangling link.
- Agent deletion cleans its skills dir along with the rest of its cascade
  (see `AGENTS.md`).

## SKILL.md format

Standard [Agent Skills](https://agentskills.io) layout — a directory with a
`SKILL.md` (YAML frontmatter + markdown body), plus optional `scripts/`,
`references/`, `resources/`, `assets/` subdirectories:

```markdown
---
name: code-review
description: Reviews code diffs for bugs and style issues
version: 1.0.0
---

# Code Review

1. Read the diff carefully
2. Flag bugs, style issues, missing tests
```

The `description` is the **trigger condition** the LLM uses to decide when
to activate the skill — write it as "when to use this", not as marketing
copy.

## Managing the library

The web **Skills** page (`#/skills`) is the library manager:

- Table of global skills with parsed name / trigger / version
- **Per-agent toggle chips** — click to enable/disable for that agent
- **+ Add Skill** modal — name (kebab-case), trigger description, markdown body
- 🗑 delete (confirms; unlinks from all agents first)

Same operations via API — see `docs/api.md` §6.

## Teaching an agent a local skill

Local skills flow the other way: you (or a routine) teach the agent, and the
agent saves what it learned into its own `.agents/skills/` via the teach
flow (`POST /api/agents/:id/skills`). Those are the agent's to keep, refine,
or forget — they never appear on the global Skills page.

## Worker agents (Phase 33)

Enabling a skill for an agent that runs on a hive-worker rig syncs the
skill directory to the rig over the worker channel and mounts it
in-container at `/skills/<name>` (read-only), linked from
`~/.agents/skills/`. Sync is automatic on enable, on `install_skill`, and
on every worker reconnect; disable/remove deletes the rig copy. The
agent's brain reads skill instructions from the server-side link either
way — heavy skill payloads (renders, builds) run on the rig.
