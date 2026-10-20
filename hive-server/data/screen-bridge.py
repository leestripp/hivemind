#!/usr/bin/env python3
"""screen-bridge.py — HiveScreen P0: CDP screencast -> WebSocket bridge.

Runs next to an agent's Chrome (bare-metal hivex user, later in-container).
Launches Chrome headless if the CDP port is dark, attaches to the newest
page target, and serves:

  GET  /status   -> JSON health (worker screen_ctl probes this)
  WS   /cast     -> screencast frames out {"type":"frame","data":<b64>,"w","h"}
                    input in {"type":"click"|"move"|"wheel"|"text"|"key"|
                              "newtab"|"retarget", ...}

First client starts the screencast; last disconnect stops it (idle-pause).
Chrome stays 127.0.0.1-only; this bridge binds 127.0.0.1 too — the worker
channel proxies UI traffic in, nothing new is exposed on the network.
"""
import argparse
import asyncio
import base64
import json
import subprocess
import sys
import time
import urllib.request

import websockets

VIEW_W, VIEW_H = 1280, 800          # deterministic viewport (1:1 frame px)
MIN_FRAME_INTERVAL = 0.15           # ~6.6fps cap via ack throttling
CHROME_ARGS = [
    "--headless=new", "--no-first-run", "--no-default-browser-check",
    "--disable-session-crashed-bubble", "--disable-dev-shm-usage",
    "--no-sandbox", "--disable-gpu", "--noerrdialogs",
    "--hide-crash-restore-bubble", "--disable-features=TranslateUI",
    f"--window-size={VIEW_W},{VIEW_H}",
]

KEY_MAP = {  # key name -> (windowsVirtualKeyCode, key, code)
    "Enter": (13, "Enter", "Enter"), "Backspace": (8, "Backspace", "Backspace"),
    "Tab": (9, "Tab", "Tab"), "Escape": (27, "Escape", "Escape"),
    "Delete": (46, "Delete", "Delete"), "Home": (36, "Home", "Home"),
    "End": (35, "End", "End"), "PageUp": (33, "PageUp", "PageUp"),
    "PageDown": (34, "PageDown", "PageDown"),
    "ArrowLeft": (37, "ArrowLeft", "ArrowLeft"),
    "ArrowRight": (39, "ArrowRight", "ArrowRight"),
    "ArrowUp": (38, "ArrowUp", "ArrowUp"),
    "ArrowDown": (40, "ArrowDown", "ArrowDown"),
}


class CDP:
    """Minimal async CDP client over one websocket."""
    def __init__(self, ws):
        self.ws = ws
        self.next_id = 0
        self.pending = {}
        self.on_event = None          # async fn(method, params)
        self._reader = asyncio.create_task(self._read_loop())

    async def _read_loop(self):
        try:
            async for raw in self.ws:
                msg = json.loads(raw)
                if "id" in msg:
                    fut = self.pending.pop(msg["id"], None)
                    if fut and not fut.done():
                        fut.set_result(msg)
                elif self.on_event:
                    # Never await here: the handler itself calls back into CDP
                    # (screencastFrameAck) and needs THIS loop to read the
                    # response. Awaiting deadlocks after the first frame.
                    asyncio.create_task(self.on_event(
                        msg.get("method", ""), msg.get("params", {})))
        except websockets.ConnectionClosed:
            for fut in self.pending.values():
                if not fut.done():
                    fut.set_exception(RuntimeError("cdp ws closed"))

    async def call(self, method, **params):
        self.next_id += 1
        mid = self.next_id
        fut = asyncio.get_running_loop().create_future()
        self.pending[mid] = fut
        await self.ws.send(json.dumps({"id": mid, "method": method, "params": params}))
        msg = await fut
        if "error" in msg:
            raise RuntimeError(f"{method}: {msg['error']}")
        return msg.get("result", {})


class Bridge:
    def __init__(self, cdp_port, chrome_bin, profile):
        self.cdp_port = cdp_port
        self.chrome_bin = chrome_bin
        self.profile = profile
        self.clients = set()
        self.cdp = None               # CDP to the attached page target
        self.browser = None           # CDP to the browser endpoint (Target.*)
        self.cast_running = False
        self.last_frame_sent = 0.0
        self.last_frame_at = time.monotonic()
        self._retarget_lock = asyncio.Lock()
        self.pending_chooser = None   # {"backendNodeId": int, "mode": str}

    # ── Chrome lifecycle ────────────────────────────────────────────────
    def _http_json(self, path):
        with urllib.request.urlopen(
                f"http://127.0.0.1:{self.cdp_port}{path}", timeout=3) as r:
            return json.load(r)

    def ensure_chrome(self):
        try:
            self._http_json("/json/version")
            return                       # already up (agent's or ours)
        except Exception:
            pass
        # Stale Singleton locks (e.g. a profile copied from another host)
        # make Chrome refuse to start — hive-chrome.sh clears these too.
        import glob as _g, os as _o
        for f in _g.glob(_o.path.join(self.profile, "Singleton*")):
            try:
                _o.remove(f)
            except OSError:
                pass
        subprocess.Popen(
            [self.chrome_bin,
             f"--remote-debugging-port={self.cdp_port}",
             f"--user-data-dir={self.profile}"] + CHROME_ARGS,
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
            start_new_session=True)
        for _ in range(50):              # up to 10s
            time.sleep(0.2)
            try:
                self._http_json("/json/version")
                return
            except Exception:
                continue
        raise RuntimeError("chrome did not come up on CDP")

    # ── Target attach ───────────────────────────────────────────────────
    async def attach_newest_page(self, target_id=""):
        ver = self._http_json("/json/version")
        if not self.browser:
            self.browser = CDP(await websockets.connect(
                ver["webSocketDebuggerUrl"], max_size=64 * 1024 * 1024))
        targets = [t for t in self._http_json("/json") if t.get("type") == "page"]
        if not targets:
            r = await self.browser.call("Target.createTarget", url="about:blank")
            target_id = r["targetId"]
            targets = [t for t in self._http_json("/json") if t.get("targetId") == target_id]
        # /json entries carry the id as "id" (createTarget returns "targetId").
        def _tid(t):
            return t.get("targetId") or t.get("id", "")
        page = next((t for t in targets if _tid(t) == target_id),
                    targets[-1])
        print(f"screen-bridge: attaching '{page.get('title','')[:40]}' "
              f"{_tid(page)[:8]}", flush=True)
        if self.cdp:
            try:
                await self.cdp.ws.close()
            except Exception:
                pass
        self.cdp = CDP(await websockets.connect(
            page["webSocketDebuggerUrl"], max_size=64 * 1024 * 1024))
        self.cdp.on_event = self._on_cdp_event
        try:
            await self.browser.call("Target.activateTarget",
                                    targetId=_tid(page))
        except Exception as e:
            print(f"screen-bridge: activateTarget: {e}", flush=True)
        await self.cdp.call("Page.enable")
        await self.cdp.call("DOM.enable")
        try:
            # Headless Chrome cannot render the native GTK file picker;
            # a page that opens one blocks forever. Intercept it and
            # satisfy it via DOM.setFileInputFiles instead (the cast page
            # relays the user's chosen file as {type:"upload"}).
            await self.cdp.call("Page.setInterceptFileChooserDialog",
                                enabled=True)
        except Exception as e:
            print(f"screen-bridge: intercept file chooser: {e}", flush=True)
        self.pending_chooser = None
        try:
            await self.cdp.call("Page.bringToFront")
        except Exception:
            pass
        await self.cdp.call("Emulation.setDeviceMetricsOverride",
                            width=VIEW_W, height=VIEW_H,
                            deviceScaleFactor=1, mobile=False)
        self.cast_running = False

    # ── Screencast ──────────────────────────────────────────────────────
    async def _on_cdp_event(self, method, params):
        try:
            await self._dispatch_event(method, params)
        except Exception as e:
            print(f"screen-bridge: event error: {e}", flush=True)

    async def _dispatch_event(self, method, params):
        if method == "Page.fileChooserOpened":
            self.pending_chooser = {
                "backendNodeId": params.get("backendNodeId"),
                "mode": params.get("mode", "selectSingle"),
            }
            print(f"screen-bridge: file chooser opened "
                  f"({self.pending_chooser['mode']})", flush=True)
            note = json.dumps({
                "type": "filechooser",
                "multiple": self.pending_chooser["mode"] != "selectSingle",
            })
            for ws in list(self.clients):
                try:
                    await ws.send(note)
                except Exception:
                    pass
            return
        if method != "Page.screencastFrame":
            return
        self.last_frame_at = time.monotonic()
        # Ack IMMEDIATELY: Chrome does not hard-gate on acks (it just
        # stops if they NEVER come), so prompt acks keep the session
        # healthy — the rate cap below is what throttles.
        try:
            await self.cdp.call("Page.screencastFrameAck",
                                sessionId=params["sessionId"])
        except Exception as e:
            print(f"screen-bridge: ACK FAILED: {e}", flush=True)
            self.cast_running = False   # watchdog restarts the session
            return
        # True rate cap: broadcast at most one frame per
        # MIN_FRAME_INTERVAL, drop the rest. Frame-on-change runs at
        # ~14fps on a busy page = ~1MB/s, which outruns the WAN splice
        # (~800KB/s) and wedges every unwindowed queue in the path —
        # proven with ss(8). 6.6fps * ~66KB = 440KB/s leaves headroom.
        now = time.monotonic()
        if now - self.last_frame_sent < MIN_FRAME_INTERVAL:
            return                      # dropped frame (acked, not sent)
        data = params.get("data", "")
        meta = params.get("metadata", {})
        frame = json.dumps({
            "type": "frame", "data": data,
            "w": VIEW_W, "h": VIEW_H,
            "scrollY": meta.get("scrollOffsetY", 0),
        })
        dead = []
        for ws in list(self.clients):
            try:
                await asyncio.wait_for(ws.send(frame), 2.0)
            except Exception as e:
                print(f"screen-bridge: DROP client ({type(e).__name__})", flush=True)
                dead.append(ws)
        for ws in dead:
            self.clients.discard(ws)
        self.last_frame_sent = time.monotonic()

    async def start_cast(self):
        if self.cast_running or not self.cdp:
            return
        await self.cdp.call("Page.startScreencast", format="jpeg",
                            quality=60, maxWidth=VIEW_W, maxHeight=VIEW_H,
                            everyNthFrame=1)
        self.cast_running = True
        print("screen-bridge: screencast started", flush=True)

    async def stop_cast(self):
        if not self.cast_running or not self.cdp:
            return
        try:
            await self.cdp.call("Page.stopScreencast")
        except Exception:
            pass
        self.cast_running = False

    async def _watchdog(self):
        # Chrome's screencast can silently stop emitting (renderer swap on
        # navigation, target crash, a lost ack) while the WS stays open —
        # the classic "connected but frozen". With clients watching and no
        # frame for 15s, bounce the session. Also self-heals cast_running
        # lying about a dead session.
        while True:
            await asyncio.sleep(5)
            if not self.clients or not self.cdp:
                continue
            if time.monotonic() - self.last_frame_at <= 15:
                continue
            print("screen-bridge: watchdog kick (no frames 15s)", flush=True)
            self.last_frame_at = time.monotonic()  # one kick per interval
            try:
                await self.cdp.call("Page.stopScreencast")
            except Exception:
                pass
            self.cast_running = False
            try:
                await self.start_cast()
            except Exception as e:
                print(f"screen-bridge: watchdog restart failed: {e}", flush=True)

    async def _keepalive(self):
        # Protocol-level ping: the browser auto-pongs, so both directions
        # stay warm and no hop (nginx, hub splice) can idle-reap the WS.
        while True:
            await asyncio.sleep(15)
            for ws in list(self.clients):
                try:
                    await ws.ping()
                except Exception:
                    pass

    # ── Input ───────────────────────────────────────────────────────────
    async def handle_input(self, msg):
        t = msg.get("type")
        c = self.cdp
        if t == "ping":
            # App-level keepalive: intermediaries can silently kill an idle
            # client->bridge leg (proven: protocol pings die after ~10s
            # idle while data flows; app pings every 5s keep it up). The
            # pong lets the page detect a half-dead path and reconnect.
            for ws in list(self.clients):
                try:
                    await ws.send(json.dumps({"type": "pong"}))
                except Exception:
                    pass
            return
        if not c:
            return
        if t == "click":
            x, y = msg["x"], msg["y"]
            count = 2 if msg.get("dbl") else 1
            await c.call("Input.dispatchMouseEvent", type="mouseMoved", x=x, y=y)
            for _ in range(count):
                await c.call("Input.dispatchMouseEvent", type="mousePressed",
                             x=x, y=y, button="left", clickCount=count)
                await c.call("Input.dispatchMouseEvent", type="mouseReleased",
                             x=x, y=y, button="left", clickCount=count)
        elif t == "move":
            await c.call("Input.dispatchMouseEvent", type="mouseMoved",
                         x=msg["x"], y=msg["y"])
        elif t == "wheel":
            await c.call("Input.dispatchMouseEvent", type="mouseWheel",
                         x=msg.get("x", VIEW_W // 2), y=msg.get("y", VIEW_H // 2),
                         deltaX=msg.get("dx", 0), deltaY=msg.get("dy", 0))
        elif t == "text":
            await c.call("Input.insertText", text=msg["text"])
        elif t == "key":
            name = msg["key"]
            if name in KEY_MAP:
                code, key, kcode = KEY_MAP[name]
                for kt in ("keyDown", "keyUp"):
                    await c.call("Input.dispatchKeyEvent", type=kt,
                                 windowsVirtualKeyCode=code, key=key, code=kcode)
            elif len(name) == 1:
                await c.call("Input.insertText", text=name)
        elif t == "upload":
            ch = self.pending_chooser
            if not ch or ch.get("backendNodeId") is None:
                return
            paths = []
            for i, f in enumerate(msg.get("files", [])):
                name = "".join(chr_ if chr_.isalnum() or chr_ in "._-"
                               else "_" for chr_ in f.get("name", f"file{i}"))[:80]
                path = f"/tmp/hive-upload-{i}-{name}"
                with open(path, "wb") as fh:
                    fh.write(base64.b64decode(f.get("data", "")))
                paths.append(path)
            if ch["mode"] == "selectSingle":
                paths = paths[:1]
            await c.call("DOM.setFileInputFiles", files=paths,
                         backendNodeId=ch["backendNodeId"])
            print(f"screen-bridge: upload -> {paths}", flush=True)
            self.pending_chooser = None
            done = json.dumps({"type": "upload_done"})
            for ws in list(self.clients):
                try:
                    await ws.send(done)
                except Exception:
                    pass
        elif t == "upload_cancel":
            ch = self.pending_chooser
            if ch and ch.get("backendNodeId") is not None:
                # Completing with an empty selection unblocks the page
                # (the input just stays empty, like pressing Cancel).
                try:
                    await c.call("DOM.setFileInputFiles", files=[],
                                 backendNodeId=ch["backendNodeId"])
                except Exception as e:
                    print(f"screen-bridge: upload cancel: {e}", flush=True)
            self.pending_chooser = None
            done = json.dumps({"type": "upload_done"})
            for ws in list(self.clients):
                try:
                    await ws.send(done)
                except Exception:
                    pass
        elif t == "navigate":
            await c.call("Page.navigate", url=msg.get("url", "about:blank"))
        elif t == "newtab":
            async with self._retarget_lock:
                await self.stop_cast()
                r = await self.browser.call("Target.createTarget",
                                            url=msg.get("url", "about:blank"))
                await self.attach_newest_page(r.get("targetId", ""))
                if self.clients:
                    await self.start_cast()
        elif t == "retarget":
            async with self._retarget_lock:
                await self.stop_cast()
                await self.attach_newest_page()
                if self.clients:
                    await self.start_cast()

    # ── WS client handling ──────────────────────────────────────────────
    async def client_loop(self, ws):
        self.clients.add(ws)
        try:
            if not self.cdp:
                await self.attach_newest_page()
            await self.start_cast()
            async for raw in ws:
                try:
                    _m = json.loads(raw)
                    print(f"screen-bridge: RX {_m.get('type')}", flush=True)
                    await self.handle_input(_m)
                except Exception as e:
                    print(f"screen-bridge: input error: {e}", flush=True)
                    await ws.send(json.dumps({"type": "error", "error": str(e)}))
        finally:
            self.clients.discard(ws)
            if not self.clients:
                await self.stop_cast()


def find_chrome():
    import glob
    import shutil
    for c in ("google-chrome", "google-chrome-stable", "chromium", "chromium-browser"):
        p = shutil.which(c)
        if p:
            return p
    # Same fallback as hive-chrome.sh: playwright-bundled chromium.
    cands = sorted(glob.glob("/opt/ms-playwright/chromium-*/chrome-linux*/chrome"))
    if cands:
        return cands[-1]
    sys.exit("screen-bridge: no chrome found")


async def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--cdp-port", type=int, default=9263)
    ap.add_argument("--serve-port", type=int, required=True)
    ap.add_argument("--profile", default="")
    args = ap.parse_args()
    profile = args.profile or __import__("os").path.expanduser("~/.browser-profile")

    bridge = Bridge(args.cdp_port, find_chrome(), profile)
    bridge.ensure_chrome()

    def process_request(connection, request):
        if request.path == "/status":
            return connection.respond(200, json.dumps({
                "ok": True, "clients": len(bridge.clients),
                "casting": bridge.cast_running, "cdp": args.cdp_port,
            }))
        return None

    async with websockets.serve(bridge.client_loop, "127.0.0.1",
                                args.serve_port, max_size=64 * 1024 * 1024,
                                process_request=process_request):
        print(f"screen-bridge: CDP :{args.cdp_port} -> ws://127.0.0.1:"
              f"{args.serve_port}/cast", flush=True)
        asyncio.create_task(bridge._watchdog())
        asyncio.create_task(bridge._keepalive())
        await asyncio.Future()           # run forever


if __name__ == "__main__":
    asyncio.run(main())
