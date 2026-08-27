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

## 4. nginx + Let's Encrypt (TLS + reverse proxy)

Two TLS endpoints behind ONE hostname (path-split — the worker channel
path is unique, no subdomain/extra DNS needed). Verified live 2026-08-27
on a DO droplet (hub.example.com).

`/etc/nginx/sites-available/hivemind`:

```nginx
# Worker channel — persistent WebSocket, hive-worker dials here
server {
    server_name hive.example.com;

    location = /api/workers/channel {
        proxy_pass http://127.0.0.1:8423;
        proxy_http_version 1.1;
        proxy_set_header Upgrade $http_upgrade;
        proxy_set_header Connection $connection_upgrade;
        proxy_read_timeout 86400s;  # the channel is persistent (15s heartbeats)
        proxy_send_timeout 86400s;
    }

    # Web UI + REST API + agent screen WebSockets
    location / {
        proxy_pass http://127.0.0.1:8421;
        proxy_set_header Host $host;
        proxy_set_header X-Forwarded-For $proxy_add_x_forwarded_for;
        proxy_set_header X-Forwarded-Proto $scheme;
        proxy_http_version 1.1;
        proxy_set_header Upgrade $http_upgrade;
        proxy_set_header Connection $connection_upgrade;
        proxy_read_timeout 3600s;   # screen sessions are long-lived
    }
}
```

The `Connection $connection_upgrade` map goes in `http {}` — on Ubuntu
add `/etc/nginx/conf.d/websocket-upgrade.conf`:

```nginx
map $http_upgrade $connection_upgrade {
    default upgrade;
    ''      close;
}
```

Then certificates + enable:

```bash
sudo apt install -y nginx certbot python3-certbot-nginx
sudo ln -s /etc/nginx/sites-available/hivemind /etc/nginx/sites-enabled/
sudo certbot --nginx -d hive.example.com
sudo systemctl reload nginx
```

Certbot rewrites the blocks for TLS and auto-renews (ports 80+443 open
in the DO cloud firewall). Point the DNS A record at the droplet first.

With nginx in front, bind hive-server to localhost so the raw ports are
unreachable from the internet:

```bash
# ~/.local/share/hivemind/server.json:
#   "worker_ws_bind": "127.0.0.1"
# and start the server with --bind 127.0.0.1
```

Workers then connect with `hive-worker --server wss://workers.example.com
--key hmwk_… --name <name>` — OmniNet's TLS client verifies the Let's
Encrypt cert against the system CA bundle (no pinning needed for public
certs; `SSL_CERT_FILE=/path/to/ca.pem` overrides for self-signed testing).

## 4b. Brute-force protection (verified live 2026-08-27)

hive-server itself has NO login throttling — a wrong key just gets 401,
unlimited attempts. Two layers fix this at the nginx edge:

**nginx rate limit** — `/etc/nginx/conf.d/rate-limit.conf`:

```nginx
limit_req_zone $binary_remote_addr zone=hivemind_api:10m rate=10r/s;
```

and inside the `location /` block:

```nginx
limit_req zone=hivemind_api burst=20 nodelay;
limit_req_status 429;
```

**fail2ban** — bans IPs with repeated 401s. Filter
`/etc/fail2ban/filter.d/hivemind-auth.conf`:

```ini
[Definition]
failregex = ^<HOST> - - \[[^\]]*\] "[A-Z]+ [^"]*" 401 .*$
ignoreregex =
```

Jail `/etc/fail2ban/jail.d/hivemind.conf`:

```ini
[hivemind-auth]
enabled  = true
port     = http,https        # web ports ONLY — SSH stays reachable
filter   = hivemind-auth
logpath  = /var/log/nginx/access.log
maxretry = 5
findtime = 600
bantime  = 3600
```

```bash
sudo apt install -y fail2ban
sudo systemctl restart fail2ban
fail2ban-client status hivemind-auth
```

Verified behavior: attempts 1–5 → 401, attempt 6 → connection dropped
(banned 1h). Parallel flood → 429 after burst=20. Unban with
`fail2ban-client unban <ip>` (or --all). Note: `/api/health` is public
so false positives are impossible — only protected routes return 401.

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

On the server itself:

```bash
cd Hive-Mind && git pull && cmake --build build -j$(nproc)
sudo systemctl restart hivemind
```

Databases migrate in place; the master key is NOT reprinted (only on a
fresh `auth.db`).

### Day-to-day rig deploy (dev-rig) — from the dev box

SSH access to dev-rig is **user `hivemind` only** — root SSH is
key-denied, and the `hivemind` user has **no sudo**. So the restart step
is always done by the human on the rig; everything else is scriptable
from the dev box:

```bash
# 1. dev box: commit, then push to the rig's bare repo (remote `rig`
#    = ssh://hivemind@dev-rig/srv/git/hivemind.git)
git push rig main

# 2. pull + rebuild on the rig (works as the hivemind user, no sudo)
ssh hivemind@dev-rig 'cd ~/hivemind && git pull --ff-only \
    && cmake --build build -j$(nproc)'

# 3. restart — as the hivemind user (2026-08-26: the rig now runs
#    SYSTEMD USER units, no root needed):
ssh hivemind@dev-rig 'systemctl --user restart hivemind-dev'
#    (hive-worker on the rig: systemctl --user restart hive-worker-dev-rig)
```

Rig unit notes (2026-08-26): server = `hivemind-dev.service`, worker =
`hive-worker-dev-rig.service`, both systemd **user** units started with
`systemd-run --user --collect` — and MUST run with CWD = `~/hivemind`
(web/dist + Containerfile paths are cwd-relative; `systemd-run` does NOT
inherit your shell's cd — use `--working-directory=` or wrap with
`bash -c 'cd ~/hivemind && exec …'`). Stop with SIGINT for graceful
agent-history save (SIGTERM's flag path has hung once).

Web-UI-only changes (`web/dist/**`) take effect on the next page load
without a restart — StaticRoutes serves them straight from the checkout
(`~/hivemind/web/dist`). The restart is only needed for C++/API changes.
`hive-deploy` (below) still does all three steps, but must be run as
root ON the rig.

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
`hive-deploy` (also captured in deploy/) = pull → rebuild → restart — run as root ON the rig (see §7 for the dev-box flow and why root/hivemind SSH can't do the restart).

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
