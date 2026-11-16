#!/usr/bin/env python3
"""Disposable native-pasta allow/deny/recreate/pin proof on one Podman rig."""
import argparse
import http.server
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import threading


class QuietHandler(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        self.send_response(200)
        self.end_headers()
        self.wfile.write(b"host-ok\n")

    def log_message(self, *_args):
        pass


def run(args, **kwargs):
    return subprocess.run(args, check=True, text=True, **kwargs)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("driver", type=Path)
    parser.add_argument("--image", default="localhost/hive-agent:latest")
    args = parser.parse_args()
    driver = args.driver.resolve()
    if not driver.is_file():
        raise SystemExit(f"driver not found: {driver}")

    agent = f"host-port-test-{os.getpid()}"
    cname = f"hive-{agent}"
    home = Path(tempfile.mkdtemp(prefix="hive-host-port-test-"))
    os.chdir(home)  # root-run SSH callers may start in an inaccessible cwd
    env = os.environ.copy()
    env["HIVE_WORKER_DIR"] = str(home / "worker-state")
    env["HIVE_SERVICE_PORT_COUNT"] = "0"
    servers = [http.server.ThreadingHTTPServer(("127.0.0.1", 0), QuietHandler)
               for _ in range(2)]
    threads = [threading.Thread(target=server.serve_forever, daemon=True)
               for server in servers]
    for thread in threads:
        thread.start()
    allowed, denied = [server.server_address[1] for server in servers]

    def ensure(mode):
        result = run([driver, agent, args.image, mode], env=env,
                     stdout=subprocess.PIPE)
        return json.loads(result.stdout.strip().splitlines()[-1])

    def probe(port, expect_allowed):
        # Expected results are explicit: a successful connection on a denied
        # port is a test failure, not merely a non-zero probe to explain away.
        script = r'''
import socket, sys, urllib.request
port, allowed = int(sys.argv[1]), sys.argv[2] == "allow"
try:
    with urllib.request.urlopen(f"http://127.0.0.1:{port}/", timeout=3) as r:
        assert r.status == 200 and r.read() == b"host-ok\n"
    connected = True
except (OSError, ValueError):
    connected = False
if connected != allowed:
    print(f"unexpected connectivity: connected={connected} allowed={allowed}", file=sys.stderr)
    sys.exit(9)
'''
        run(["podman", "exec", cname, "python3", "-c", script,
             str(port), "allow" if expect_allowed else "deny"],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE)

    try:
        initial = ensure("")
        assert initial["network"] == "pasta", initial
        probe(allowed, False)
        probe(denied, False)

        granted = ensure(f"{allowed},{allowed}")  # duplicate normalization too
        assert granted["network"] == f"pasta:-T,{allowed}", granted
        assert granted["id"] != initial["id"]
        assert granted["image"] == initial["image"]  # no image replacement/build
        probe(allowed, True)
        probe(denied, False)

        unchanged = ensure(str(allowed))
        assert unchanged["id"] == granted["id"]
        probe(allowed, True)

        # A separate driver process with no spec exercises the persisted pin.
        pinned = ensure("pin")
        assert pinned["id"] == granted["id"]
        assert pinned["network"] == granted["network"]
        probe(allowed, True)
        probe(denied, False)

        run(["podman", "stop", cname], stdout=subprocess.PIPE)
        restarted = ensure("pin")
        assert restarted["id"] == granted["id"]  # stop/start, not recreate
        probe(allowed, True)

        revoked = ensure("")
        assert revoked["network"] == "pasta", revoked
        assert revoked["id"] != granted["id"]
        assert revoked["image"] == initial["image"]
        probe(allowed, False)
        probe(denied, False)

        # A foreign network mode must fail closed and leave the existing
        # container intact; policy reconciliation may never silently replace it.
        run(["podman", "rm", "-f", cname], stdout=subprocess.PIPE)
        run(["podman", "create", "--name", cname, "--network=host",
             args.image], stdout=subprocess.PIPE)
        foreign_id = run(["podman", "inspect", "-f", "{{.Id}}", cname],
                         stdout=subprocess.PIPE).stdout.strip()
        rejected = subprocess.run([driver, agent, args.image, ""], env=env,
                                  text=True, stdout=subprocess.PIPE,
                                  stderr=subprocess.PIPE)
        assert rejected.returncode != 0, rejected.stdout
        assert "custom networking" in rejected.stderr, rejected.stderr
        assert run(["podman", "inspect", "-f", "{{.Id}}", cname],
                   stdout=subprocess.PIPE).stdout.strip() == foreign_id

        print(json.dumps({"ok": True, "agent": agent,
                          "allowed_port": allowed, "denied_port": denied,
                          "checks": ["default-deny", "selective-allow",
                                     "negative-port", "unchanged-id",
                                     "process-restart-pin", "stop-start",
                                     "revocation", "image-preserved",
                                     "foreign-network-preserved"]}))
    finally:
        subprocess.run(["podman", "rm", "-f", cname],
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        for server in servers:
            server.shutdown()
            server.server_close()
        shutil.rmtree(home, ignore_errors=True)


if __name__ == "__main__":
    main()
