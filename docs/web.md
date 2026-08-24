# Web dashboard — the browser UI

> hive-server serves a zero-build static web UI from `web/dist/` at
> `http://127.0.0.1:8421/`. It is the management console: roster, tasks,
> skills, models, keys. Chat-first daily driving is the GTK client's job
> (`docs/client.md`); the web UI covers configuration and oversight.

---

## Stack

Plain ES modules — no bundler, no framework:

```
web/dist/
├── index.html            # shell: activity bar, page divs, modals
├── css/base.css          # all styles (dark theme, CSS variables)
├── js/main.js            # boot + hash router (#/agents, #/tasks, …)
├── js/api.js             # fetch wrapper (API key auth header)
├── js/util.js            # $, esc, log helpers
└── js/pages/             # one module per page: agents, tasks, skills,
                          # models, keys, settings
```

Every page module exports `init<Page>()` (wire events once) and
`enter<Page>()` (load data when navigated to). Cache-busting is manual:
bump the `?v=` query in `index.html` when JS/CSS changes.

Authentication: the server requires an API key (`Authorization: Bearer
hm_…`); the web UI prompts once and stores it in `localStorage`.

## Pages

### Agents (`#/agents`)

The roster as cards: avatar, name, role, model, approval mode. Actions:
create agent, start/stop container, spawn (installs daedalus + writes the
agent's `AGENTS.md`). Deletion lives in the GTK client (right-click → Delete
Agent shows the cascade summary; see `docs/client.md`). Agent chat opens in
the GTK client too; the web UI is roster management only.

### Scheduled Tasks (`#/tasks`)

Dashboard for routines (the server's 30 s scheduler tick):

- Table sorted active-first: name/prompt, agent, human-readable schedule,
  next/last fire, status badge (`active`, `paused`, `✓ done` for fired
  one-shots)
- Row actions: enable/pause toggle, ▶ run-now (off-schedule), 🗑 delete
  (also deletes the routine's thread)
- **+ Add Task** modal: agent picker, prompt, dynamic schedule fields —
  once (datetime) / interval (minutes) / daily (time) / weekly (time +
  Su–Sa day picker)
- **🧹 Cleanup Completed** — bulk-deletes fired one-shot tasks

### Skills (`#/skills`)

Global skill library manager — see `docs/skills.md` for the design.
Table with per-agent toggle chips, + Add Skill modal, delete (unlinks from
all agents first).

### Models (`#/models`)

LLM providers and their models: add provider (name, host, API key,
timeout), add model per provider, delete. Backed by the server's LLMStore
(`~/.local/share/hivemind/llm.db`) — the same source the GTK client's
New Agent dialog reads for its Provider/Model dropdowns.

### API Keys (`#/keys`)

Manage the `hm_…` bearer keys the API requires: create (shown once),
revoke. The web UI's own key lives in `localStorage` after first prompt.

### Settings (`#/settings`)

Server config view.

## Adding a new page

1. `web/dist/js/pages/foo.js` — export `initFoo()` + `enterFoo()`
2. `index.html` — activity-bar button (`data-page="foo"`), `#page-foo` div,
   any modals
3. `main.js` — import, add `'foo'` to `PAGES`, wire `showPage()` + `initFoo()`
4. Bump `?v=` cache-bust in `index.html`
