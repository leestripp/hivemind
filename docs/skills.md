# Skills (v2) — one library, everywhere

Skills are Anthropic-style capability packages (SKILL.md + scripts and
resources). In v2 the hub owns the canonical library and every agent sees
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
- Bind-mounts it read-only into every agent container at hive-brain'
  global skills dir (`Paths::global().global_skills_dir()` — the XDG
  path hive-brain scans automatically).
- Per-agent enable/disable is roster metadata; the hub passes each
  agent's enabled list at spawn (mount subset or env filter — hive-brain
  `skills` config).
- Additionally, `/workspace/.agents/skills/` (project-local) is scanned
  automatically by every agent whose project root is the shared
  /workspace — agents can create and share skills among themselves with
  no hub involvement.

## Why this just works now

In v1 the brain was on the server and skill SCRIPTS had to execute in a
remote container — hence streaming sync and symlink surgery. In v2 the
brain is in the container, next to `/skills`. A skill that says "run
`render.sh`" works: the script, the shell, and the agent are one place.
