# Skills — one library, everywhere

Skills are Anthropic-style capability packages (SKILL.md + scripts and
resources). The hub owns the canonical library and every agent sees
the same skills — because the skill files sit at hive-brain' known global
location inside every container.

## The Skills Manager (hub side)

- Canonical library: `~/.local/share/hivemind/skills/` on the hub.
- Discovery + install: the `search_skills` / `install_skill` agent tools
  and the client's Skills page operate on this library (unchanged from
  v1 — registry of curated skills, install on demand).
- Sync: when a worker connects (and whenever the library changes), the
  hub pushes the library over the worker channel (`skill_sync` begin/
  file/end/remove ops — the Phase 33 transport, kept). Delta sync by
  content hash so reconnects are cheap.

## The worker side

- Lands the library at `~/.local/share/hive-worker/skills/`.
- Bind-mounts it read-only into every agent container at `/skills`
  (`HIVEMIND_SKILLS_DIR`). Agents **cannot** modify it — this is the
  built-in, hub-managed tier.
- Per-agent enable/disable is roster metadata; the hub passes each
  agent's enabled list at spawn (env filter — hive-brain `skills`
  config).

## The workspace global tier (§Skills-Global, 2026-09-30)

`/workspace/.agents/skills/` is THE writable global skill library,
shared by every agent on the worker via the shared `/workspace` mount.
Agents create shared skills here themselves — write
`<skill-name>/SKILL.md`, no hub involvement, no restart:

- **Lazy-loaded at runtime**: `discover_skills` performs a live rescan
  on every call, the per-turn system-prompt catalog refreshes when the
  cache is older than 30 s (`maybe_refresh_cache`), and `load_skill`
  reads disk first — so creations AND edits are picked up immediately.
- **Skip reasons are surfaced**: a directory dropped from the catalog
  (no SKILL.md, bad frontmatter, missing name/description) appears in
  `discover_skills`' `skipped` list with the reason — never silent.
- **Priority**: the workspace global is scanned after `/skills`, so a
  workspace skill shadows a same-named library skill (deterministically,
  even under parallel discovery).
- **Per-rig**: like the rest of `/workspace`, each worker has its own
  independent global tier. There is no cross-rig sync — that is by
  design.
- `~/.agents/skills/` (container home) remains on the scan list as a
  PRIVATE per-agent tier, but it dies with the container — anything
  shared must go to `/workspace/.agents/skills/`.

## Why this just works now

In v1 the brain was on the server and skill SCRIPTS had to execute in a
remote container — hence streaming sync and symlink surgery. The
brain is in the container, next to `/skills`. A skill that says "run
`render.sh`" works: the script, the shell, and the agent are one place.
