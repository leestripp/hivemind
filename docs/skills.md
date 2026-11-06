# Skills — curated starters plus agent-owned skills

Skills are Anthropic-style capability packages (`SKILL.md` plus optional
scripts and resources). HiveMind has one simple hub action: **install a curated
starter for every agent**. There are no per-agent enable switches.

## Curated starters

The web UI's **Skills** page lists only the curated registry. **Install for all
agents** clones that entry on the hub, validates its `SKILL.md`, and streams a
copy to each connected worker. Workers install missing copies in:

```
/workspace/.agents/skills/<skill-name>/
```

That is the shared, writable workspace tier. Every agent on the worker sees the
starter without a restart. An offline enrolled worker receives missing starters
on reconnect, before the hub spawns its agents.

These are **starting copies, not a read-only library**. Ask an agent to correct
or improve one and it can edit the shared copy through its normal tools. A
reconnect never overwrites an existing copy, including an agent-edited one.
The hub's `~/.local/share/hivemind/skills/` directory is only an internal
installation cache; it is not an inventory of agents' skills.

**Uninstall** removes that starter's shared workspace copies. It requires every
enrolled worker to be online so the hub cannot silently strand copies on an
offline machine. Private agent skills are never touched.

## Skills agents own

Agents can create and improve skills themselves through chat:

- `/workspace/.agents/skills/` — shared by every agent on that worker and
  writable by all of them. Use this for a team-wide skill or correction.
- `~/.agents/skills/` in the agent's home — private to that agent.

The hub deliberately does **not** list, enable, disable, or synchronize these
agent-owned skills. Ask the agent that owns them, or an agent on the relevant
worker for a shared workspace skill.

Image-baked built-ins remain available from
`/usr/local/share/hive-agent/skills`. Discovery rescans the filesystem, so new
and edited skills are picked up live. If two directories contain the same skill
name, the later directory wins. HiveMind's order is:

1. optional explicit `HIVEMIND_SKILLS_DIR` entries;
2. image-baked built-ins;
3. shared workspace skills (including installed starters);
4. private home skills.

Thus a workspace correction overrides a built-in or explicitly supplied starter,
and a private copy has the final say.

The former read-only `/skills` library mount and hub-computed per-agent disabled
list are retired. Old files under the worker's `skills/` directory are left in
place for inspection but are no longer mounted or synchronized.

## HTTP API

| Endpoint | Purpose |
|----------|---------|
| `GET /api/skills/registry` | Curated catalog with each entry's installed state |
| `GET /api/skills` | Installed curated starters only |
| `POST /api/skills/install` | Body: `{"name":"catalog-entry"}`; install for all agents |
| `DELETE /api/skills/:name` | Uninstall a curated starter for all agents |

Arbitrary Git URLs, hand-authored hub skills, workspace inventory, and per-agent
`/global-skills` toggles are intentionally not part of this API.
