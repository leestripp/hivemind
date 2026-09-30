---
name: mcp-self-service
description: "Install, register, enable, disable and remove MCP servers yourself inside your container (mcp_add/mcp_list/mcp_enable/mcp_disable/mcp_remove). Use when the user asks you to set up or remove an MCP server (e.g. comfy-mcp), when you need a capability no builtin tool covers, or when an MCP server misbehaves and needs reconnecting."
---

# MCP Self-Service

You manage your OWN MCP servers. No human, no hub API calls, no restart —
the tools below take effect live and persist across container recreations.

## The tools

- `mcp_list` — what's configured: name, origin, enabled, connected, tools.
  `origin=hub` rows come from the user via the hub roster — READ-ONLY to
  you (mcp_enable for a reconnect is fine; disable/remove are refused).
  `origin=local` rows are yours. Env values are redacted (keys only).
- `mcp_add(name, command, args, env, url, description)` — register a NEW
  server and connect immediately. Persists as origin=local. Returns the
  discovered tool list on success.
- `mcp_enable(name)` / `mcp_disable(name)` — connect/disconnect; the state
  survives restarts.
- `mcp_remove(name)` — disconnect + delete the registration (local only).
  Does not uninstall the server software.

## The loop (follow it in order)

1. **Install** with the bash tool — the server must exist INSIDE your
   container. Prefer `~/.local` or `/workspace` for installs (both
   persist; image dirs do not):
   - python: `pip install --user <pkg>` or `uv tool install <pkg>`
   - node: `npm install -g <pkg>` (or `npx <pkg>` on demand)
   - source: `git clone … /workspace/<pkg> && build per its README`
   Then verify the binary runs: `command -v <bin>` or `<bin> --help`.
2. **Register**: `mcp_add` with the command/args (stdio) or url (http).
   Use a short lowercase name — its tools appear as `mcp__<name>__<tool>`.
3. **Verify**: check `mcp_add`'s returned tool list, then make ONE real
   `mcp__<name>__<tool>` call with harmless arguments. If the connection
   failed, read `error`, fix the command/url, and `mcp_enable` to retry
   (or `mcp_remove` and re-add).
4. **Document**: if the server is genuinely useful, write a short skill
   to the workspace global skills dir (`/workspace/.agents/skills/<name>/SKILL.md`)
   so teammates on this rig can repeat the setup — include the exact
   install command and the `mcp_add` arguments.

## Worked example: comfy-mcp

```bash
# 1. install + verify (adjust to the real package you chose)
pip install --user comfy-mcp
command -v comfy-mcp
```
```json
// 2. register (ComfyUI reachable at http://localhost:8188 from the container)
mcp_add {"name": "comfy", "command": "comfy-mcp",
         "env": {"COMFYUI_URL": "http://localhost:8188"},
         "description": "ComfyUI image/video generation"}
```
```json
// 3. verify — list shows connected:true + tools; then one real call:
mcp__comfy__<some_read_only_tool> {...}
```

## Traps

- **Install location**: `/usr/local/bin` inside the container is the
  IMAGE — gone on recreation. `~/.local`, `/workspace` persist.
- **PATH**: stdio spawn uses your container PATH; if the binary is in
  `~/.local/bin` either use the absolute path in `command` or confirm
  `~/.local/bin` is on PATH (`echo $PATH`).
- **Secrets** go in `env` — stored container-locally, redacted in
  mcp_list, never sent to the hub. Do not put secrets in `args`
  (they'd be visible in mcp_list).
- **Don't fight the roster**: an origin=hub server you dislike stays —
  ask the user. Yours are origin=local.
- **Verify before trusting**: a server that connects can still have
  broken tools. One real call, always.
