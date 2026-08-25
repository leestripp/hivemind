# Remote deployment — hive-server on a droplet/VPS

Target: a Linux server (tested on Ubuntu 26.04) reachable over the public
internet. TLS is terminated by **Caddy** in front of hive-server — OmniNet
speaks plain HTTP only; never expose :8421 raw.

## 1. Provision

```bash
# 4–8 GB RAM recommended (Chromium in agent containers is the big eater)
sudo apt update && sudo apt install -y podman caddy build-essential cmake git

git clone https://github.com/<you>/Hive-Mind.git && cd Hive-Mind
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```

## 2. First run — bootstrap the master key

```bash
./build/hive-server/hive-server --port 8421 --bind 127.0.0.1
# prints the master API key ONCE — save it; auth.db stores only its hash
```

State lives in `~/.local/share/hivemind/` — `auth.db` (API keys),
`agents.db`/`threads.db`, `llm.db` (provider keys), `secrets.db` +
`secrets.key` (secret broker), `server.json` (config), `agents/` (agent
homes), `workspace/` (company share). Back up this whole directory;
`secrets.key` is required to decrypt `secrets.db`.

**Relocating the workspace**: the company share can live on bulk storage —
`PUT /api/config {"workspace_root": "/path"}` (Settings page works too).
Containers mount it at `/workspace` (plain bind, no `:U`); the dir needs to
be writable by BOTH the service user and subuid-mapped container users —
use mode `0777` (NO sticky bit: sticky EPERMs cross-deletes) and the server
creates its `attachments/<thread>/` dirs as 0777 too (FileRoutes).
**Do NOT use default POSIX ACLs on btrfs** — its default-ACL inheritance
bakes setuid+setgid+sticky (mode 7777) into every new subdirectory, which
then blocks cross-unlinks with "Operation not permitted". Rig layout
(dev-rig): the 2×4TB btrfs RAID1 is mounted at `/workspace` (fstab by UUID,
`nosuid,nodev,nofail`), and hivemind's `workspace_root` points there.

## 3. systemd unit

`/etc/systemd/system/hivemind.service`:

```ini
[Unit]
Description=HiveMind server
After=network.target

[Service]
Type=simple
User=ubuntu
WorkingDirectory=/home/ubuntu/Hive-Mind
ExecStart=/home/ubuntu/Hive-Mind/build/hive-server/hive-server --port 8421 --bind 127.0.0.1
Restart=on-failure
RestartSec=3

[Install]
WantedBy=multi-user.target
```

```bash
sudo systemctl enable --now hivemind
journalctl -u hivemind -f
```

Rootless podman note: the service user needs `loginctl enable-linger ubuntu`
so user-scope podman survives logout, and containers run as that user.

## 4. Caddy (TLS + reverse proxy)

`/etc/caddy/Caddyfile`:

```
hive.example.com {
    reverse_proxy 127.0.0.1:8421
}
```

```bash
sudo systemctl reload caddy
```

Caddy obtains/renews the certificate automatically (ports 80+443 must be
open in the firewall/DO cloud firewall). WebSocket routes (`/ws/*`) work
through `reverse_proxy` with no extra config.

## 5. Lock down

```bash
sudo ufw allow 22,80,443/tcp && sudo ufw enable
# :8421 stays on localhost — only Caddy reaches it
```

- API keys: `POST /api/keys` (admin scope) — one key per client/user;
  `agent` scope for everyday clients, `read` for dashboards.
- CORS: only needed if a browser app on ANOTHER origin calls the API —
  edit `~/.local/share/hivemind/server.json`:
  `"cors_origins": ["https://your-app.example.com"]` (or `"*"`, not
  recommended) and restart. The built-in web UI is same-origin and needs
  nothing.
- Secrets: add credentials via the web UI Secrets page (`#/secrets`) —
  write-only, AES-256-GCM at rest, delivered to granted agents as
  `HIVE_SECRET_<NAME>` env vars without entering the LLM context.

## 6. Clients

- **Web UI**: `https://hive.example.com/` — enter the API key once.
- **GTK client**: Settings → server URL `https://hive.example.com`, key.

## 7. Upgrades

```bash
cd Hive-Mind && git pull && cmake --build build -j$(nproc)
sudo systemctl restart hivemind
```

Databases migrate in place; the master key is NOT reprinted (only on a
fresh `auth.db`).

## 8. Backups (hive-backup)

`/usr/local/bin/hive-backup` (source: `hive-server/deploy/`) runs daily at
03:30 via `hive-backup.timer`:

- **Online SQLite backups** (`.backup`) of every `*.db` in
  `~/.local/share/hivemind/` — safe while the server is writing
- **Hardlink-incremental rsync** of everything else (agent homes, skills,
  server.json, the secrets key) — unchanged files cost no space
- Destination `/workspace/backups/hivemind/<timestamp>/` (btrfs RAID1,
  root-only 0700 — it contains secrets), 14-day retention

Manual run: `sudo hive-backup`. Restore: stop hivemind, copy a snapshot's
`db/*.db` + `files/` back over `~/.local/share/hivemind/`, start hivemind.
The agents' *containers* are not backed up — they're disposable; rebuild
from the Containerfile.

## 9. Packaging (Phase 31)

### Fresh server install
`hive-server/deploy/install.sh` (run as root, path to checkout):
creates the `hivemind` user (+ linger for rootless podman), copies the
checkout to `~hivemind/hivemind`, installs the systemd units
(hive-server.service, hive-backup.timer), installs hive-deploy +
hive-backup to /usr/local/bin, builds, enables, health-checks.
`hive-deploy` (also captured in deploy/) = pull → rebuild → restart.

### Flatpak client
`hive-client/packaging/org.hivemind.Client.yml` — GNOME 48 runtime,
builds the gtkmm-4.0 stack + nlohmann_json + OmniNet from source
(client-only configure: HIVEMIND_BUILD_SERVER=OFF /
HIVEMIND_USE_DAEDALUS=OFF, FETCHCONTENT_SOURCE_DIR_OMNINET points at
the pre-fetched module). Desktop file + metainfo + icon included.

    flatpak-builder --force-clean build-dir \
        hive-client/packaging/org.hivemind.Client.yml

Not build-tested in the dev sandbox (no flatpak-builder) — run on a
machine with flatpak + the GNOME 48 SDK installed.
