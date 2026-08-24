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
mode `1777` (sticky) works: service writes its files, containers write
theirs, nobody deletes each other's (the files API is read-only anyway).
Rig layout (dev-rig): the 2×4TB btrfs RAID1 is mounted at `/workspace`
(fstype btrfs, fstab by UUID, `nosuid,nodev,nofail`), and hivemind's
`workspace_root` points there.

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
