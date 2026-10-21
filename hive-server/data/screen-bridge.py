#!/usr/bin/env python3
"""screen-bridge.py — HiveScreen v2: CDP screen share bridge.

Runs next to an agent's Chrome (bare-metal hivex user, later in-container).
Launches Chrome headless if the CDP port is dark, attaches to a page
target, and serves:

  GET  /status   -> JSON health (worker screen_ctl probes this)
  WS   /cast     -> frames out  {"type":"frame","data":<b64 jpeg>,"w","h"}
                    tabs out    {"type":"tabs","tabs":[{id,title,url,active}]}
                    input in    {"type":"click"|"dblclick"|"move"|"wheel"|
                                 "text"|"key"|"navigate"|"newtab"|
                                 "switchtab"|"closetab"|"upload"|
                                 "upload_cancel"|"ping", ...}

v2 (2026-10-21, full refactor per lee after v1's black-window/stale-
live/invisible-tabs failures):

  * GUARANTEED FRAMES. v1 relied solely on Page.startScreencast, which in
    headless Chrome emits only on damage of the compositing target — a
    static or background tab means NO frames (black canvas, "live" that
    lies). v2 is hybrid: screencast for busy pages PLUS a heartbeat
    Page.captureScreenshot whenever no frame has arrived for 0.8s, PLUS
    an input-boost capture 150ms after every input event, PLUS an
    immediate capture on every attach/switch. A watched page always
    streams >=1fps; identical frames are md5-deduped so idle pages cost
    ~zero bandwidth.
  * LIVE TARGET MODEL. Target.setDiscoverTargets on the browser socket
    keeps an always-current {id,title,url} tab list; changes are pushed
    to clients as "tabs" messages. current_target is explicit — never
    guessed from /json ordering (v1 attached to "newest", which could be
    a background tab that never composites -> black window).
  * QUIET LOGS. Tab switches, cast restarts, client join/part, errors —
    no per-input RX spam.

Chrome stays 127.0.0.1-only; this bridge binds 127.0.0.1 too — the worker
channel proxies UI traffic in, nothing new is exposed on the network.
"""
import argparse
import asyncio
import base64
import hashlib
import json
import subprocess
import sys
import time
import urllib.request

import websockets

VIEW_W, VIEW_H = 1280, 800          # deterministic viewport (1:1 frame px)
MIN_FRAME_INTERVAL = 0.15           # ~6.6fps cap on screencast frames
HEARTBEAT_AFTER = 0.8               # capture if no frame for this long
HEARTBEAT_TICK = 0.5                # heartbeat loop cadence
INPUT_BOOST_DELAY = 0.15            # capture this long after input
WATCHDOG_STALL = 5.0                # no frames this long -> restart cast
JPEG_QUALITY = 45               # was 60: halve wire bytes for slow viewers
CAPTURE_MIN_INTERVAL = 0.4      # heartbeat/boost frames can't burst past this
FORCE_FRAME_EVERY = 4.0         # send even an identical frame this often
CDP_CALL_TIMEOUT = 8.0          # a stalled page must never wedge input

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


def log(msg):
    print(f"screen-bridge: {msg}", flush=True)


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
                    # Never await here: handlers call back into CDP
                    # (screencastFrameAck) and need THIS loop alive to
                    # read the response. Awaiting deadlocks.
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
        await self.ws.send(json.dumps({"id": mid, "method": method,
                                       "params": params}))
        try:
            msg = await asyncio.wait_for(fut, CDP_CALL_TIMEOUT)
        except asyncio.TimeoutError:
            self.pending.pop(mid, None)
            raise RuntimeError(f"{method}: CDP call timed out") from None
        except BaseException:
            self.pending.pop(mid, None)   # cancelled: don't leak
            raise
        if "error" in msg:
            raise RuntimeError(f"{method}: {msg['error']}")
        return msg.get("result", {})


class TargetBook:
    """Live model of the browser's page targets. No I/O — unit-testable.

    Fed by Target.setDiscoverTargets events:
      Target.targetCreated      {targetInfo:{targetId,type,title,url}}
      Target.targetInfoChanged  {targetInfo:{...}}   (title/url updates)
      Target.targetDestroyed    {targetId}
    """
    def __init__(self):
        self.targets = {}             # id -> {"id","title","url"}
        self.order = []               # target ids, creation order

    def apply(self, method, params):
        """Apply one Target.* event. Returns True if the model changed."""
        if method == "Target.targetCreated":
            info = params.get("targetInfo", {})
            if info.get("type") != "page":
                return False
            tid = info.get("targetId", "")
            if not tid or tid in self.targets:
                return False
            self.targets[tid] = {"id": tid,
                                 "title": info.get("title", ""),
                                 "url": info.get("url", "")}
            self.order.append(tid)
            return True
        if method == "Target.targetInfoChanged":
            info = params.get("targetInfo", {})
            tid = info.get("targetId", "")
            t = self.targets.get(tid)
            if not t:
                return False
            title, url = info.get("title", ""), info.get("url", "")
            if title == t["title"] and url == t["url"]:
                return False
            if title:
                t["title"] = title
            if url:
                t["url"] = url
            return True
        if method == "Target.targetDestroyed":
            tid = params.get("targetId", "")
            if tid not in self.targets:
                return False
            del self.targets[tid]
            self.order = [x for x in self.order if x != tid]
            return True
        return False

    def tabs(self, current_id):
        return [{"id": t["id"], "title": t["title"] or t["url"] or "tab",
                 "url": t["url"], "active": t["id"] == current_id}
                for t in (self.targets[i] for i in self.order
                          if i in self.targets)]

    def newest(self):
        for tid in reversed(self.order):
            if tid in self.targets:
                return tid
        return ""


class Bridge:
    def __init__(self, cdp_port, chrome_bin, profile):
        self.cdp_port = cdp_port
        self.chrome_bin = chrome_bin
        self.profile = profile
        self.clients = set()
        self.cdp = None               # CDP to the attached page target
        self.browser = None           # CDP to the browser endpoint
        self.book = TargetBook()
        self.current_target = ""
        self.cast_running = False
        self.last_frame_at = time.monotonic()
        self.last_frame_sent = 0.0
        self.last_frame_hash = ""
        self.pending_chooser = None   # {"backendNodeId": int, "mode": str}
        self._retarget_lock = asyncio.Lock()
        self._capture_lock = asyncio.Lock()
        self._boost_handle = None
        self.last_capture_sent = 0.0
        self.last_frame_wire = 0.0
        self._move_pending = None   # latest (x,y) awaiting coalesced dispatch
        self._move_task = None

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
    async def _ensure_browser(self):
        if self.browser:
            return
        ver = self._http_json("/json/version")
        self.browser = CDP(await websockets.connect(
            ver["webSocketDebuggerUrl"], max_size=64 * 1024 * 1024))
        self.browser.on_event = self._on_browser_event
        await self.browser.call("Target.setDiscoverTargets", discover=True)

    async def attach(self, target_id=""):
        """Attach the page socket to target_id (or the newest tab)."""
        await self._ensure_browser()
        if not target_id:
            target_id = self.book.newest()
        page_url = ""
        if not target_id or target_id not in self.book.targets:
            # Book empty (discovery still catching up) — fall back to
            # /json, creating a tab if the browser has none at all.
            targets = [t for t in self._http_json("/json")
                       if t.get("type") == "page"]
            if not targets:
                r = await self.browser.call("Target.createTarget",
                                            url="about:blank")
                target_id = r["targetId"]
            else:
                target_id = target_id or \
                    (targets[-1].get("targetId") or targets[-1].get("id"))
        for t in self._http_json("/json"):
            if (t.get("targetId") or t.get("id")) == target_id:
                page_url = t["webSocketDebuggerUrl"]
                break
        if not page_url:
            raise RuntimeError(f"target {target_id[:8]} has no page socket")
        self.current_target = target_id
        if self.cdp:
            try:
                await self.cdp.ws.close()
            except Exception:
                pass
        self.cdp = CDP(await websockets.connect(
            page_url, max_size=64 * 1024 * 1024))
        self.cdp.on_event = self._on_page_event
        # Activation discipline: the target must be the compositing one
        # in headless or it never renders (v1's black window).
        try:
            await self.browser.call("Target.activateTarget",
                                    targetId=target_id)
        except Exception as e:
            log(f"activateTarget: {e}")
        await self.cdp.call("Page.enable")
        await self.cdp.call("DOM.enable")
        try:
            # Headless Chrome cannot render the native GTK file picker;
            # intercept and satisfy via DOM.setFileInputFiles instead
            # (the cast page relays the user's chosen file).
            await self.cdp.call("Page.setInterceptFileChooserDialog",
                                enabled=True)
        except Exception as e:
            log(f"intercept file chooser: {e}")
        self.pending_chooser = None
        try:
            await self.cdp.call("Page.bringToFront")
        except Exception:
            pass
        await self.cdp.call("Emulation.setDeviceMetricsOverride",
                            width=VIEW_W, height=VIEW_H,
                            deviceScaleFactor=1, mobile=False)
        self.cast_running = False
        self.last_frame_hash = ""       # new target: send the first frame
        title = self.book.targets.get(target_id, {}).get("title", "")
        log(f"attached '{title[:50]}' {target_id[:8]}")
        await self._broadcast_tabs()

    # ── Frame engine (hybrid: screencast + heartbeat + boost) ───────────
    async def _broadcast(self, msg):
        dead = []
        for ws in list(self.clients):
            try:
                await asyncio.wait_for(ws.send(msg), 2.0)
            except Exception:
                dead.append(ws)
        for ws in dead:
            self.clients.discard(ws)
            log("drop slow client")

    async def _broadcast_frame(self, data_b64, scroll_y=0.0):
        """Dedup + send. Returns True if a frame actually went out."""
        now = time.monotonic()
        self.last_frame_at = now   # pipeline alive even when deduped
        h = hashlib.md5(data_b64.encode()).hexdigest()
        if h == self.last_frame_hash and \
                now - self.last_frame_wire < FORCE_FRAME_EVERY:
            return False                # identical to previous — skip
        # Identical frames still go out every FORCE_FRAME_EVERY so a
        # healthy static page reads LIVE (not STALE/FROZEN) and the
        # watchdog stays quiet.
        self.last_frame_hash = h
        self.last_frame_wire = now
        await self._broadcast(json.dumps({
            "type": "frame", "data": data_b64,
            "w": VIEW_W, "h": VIEW_H, "scrollY": scroll_y}))
        return True

    async def _capture_once(self, reason):
        """One Page.captureScreenshot of the active target."""
        if not self.cdp or not self.clients:
            return
        # Rate-cap capture-sourced frames: the screencast is capped at
        # MIN_FRAME_INTERVAL but heartbeat/boost used to bypass it, so
        # heavy interaction on an image-heavy page burst past the WAN
        # splice's drain rate (lee's mouse-death, 2026-10-21).
        if reason != "watchdog" and \
                time.monotonic() - self.last_capture_sent < CAPTURE_MIN_INTERVAL:
            return
        async with self._capture_lock:
            try:
                r = await self.cdp.call("Page.captureScreenshot",
                                        format="jpeg",
                                        quality=JPEG_QUALITY)
            except Exception as e:
                log(f"capture ({reason}) failed: {e}")
                self.last_frame_at = 0.0   # let the watchdog react
                return
        if await self._broadcast_frame(r.get("data", "")):
            self.last_capture_sent = time.monotonic()

    async def _heartbeat(self):
        # Static pages emit no screencast damage (v1: frame counter
        # stalls for minutes, "live" indistinguishable from wedged).
        # If no frame of any kind arrived recently while clients watch,
        # pull one ourselves.
        while True:
            await asyncio.sleep(HEARTBEAT_TICK)
            if not self.clients or not self.cdp:
                continue
            if time.monotonic() - self.last_frame_at > HEARTBEAT_AFTER:
                await self._capture_once("heartbeat")

    def _boost(self):
        """Schedule a capture shortly after an input event."""
        if self._boost_handle:
            self._boost_handle.cancel()
        async def _later():
            await asyncio.sleep(INPUT_BOOST_DELAY)
            await self._capture_once("input-boost")
        self._boost_handle = asyncio.ensure_future(_later())

    def _queue_move(self, x, y):
        """Coalesce mousemoves: only the latest position matters, and a
        heavy page (100ms+ per dispatch) must never let queued moves
        delay a click sitting behind them."""
        self._move_pending = (x, y)
        if self._move_task is None or self._move_task.done():
            self._move_task = asyncio.ensure_future(self._move_worker())

    async def _move_worker(self):
        while self._move_pending is not None:
            x, y = self._move_pending
            self._move_pending = None
            if not self.cdp:
                return
            try:
                await self.cdp.call("Input.dispatchMouseEvent",
                                    type="mouseMoved", x=x, y=y)
            except Exception:
                return

    async def _on_browser_event(self, method, params):
        try:
            if self.book.apply(method, params):
                await self._broadcast_tabs()
                # Current tab closed under us -> attach the newest
                # remaining one so the view never dies.
                if method == "Target.targetDestroyed" and \
                        params.get("targetId") == self.current_target:
                    nxt = self.book.newest()
                    if nxt:
                        async with self._retarget_lock:
                            await self.stop_cast()
                            await self.attach(nxt)
                            if self.clients:
                                await self.start_cast()
        except Exception as e:
            log(f"browser event error: {e}")

    async def _on_page_event(self, method, params):
        try:
            await self._dispatch_page_event(method, params)
        except Exception as e:
            log(f"page event error: {e}")

    async def _dispatch_page_event(self, method, params):
        if method == "Page.fileChooserOpened":
            self.pending_chooser = {
                "backendNodeId": params.get("backendNodeId"),
                "mode": params.get("mode", "selectSingle"),
            }
            log(f"file chooser opened ({self.pending_chooser['mode']})")
            await self._broadcast(json.dumps({
                "type": "filechooser",
                "multiple": self.pending_chooser["mode"] != "selectSingle"}))
            return
        if method != "Page.screencastFrame":
            return
        # Ack IMMEDIATELY: Chrome stops emitting if acks never come.
        try:
            await self.cdp.call("Page.screencastFrameAck",
                                sessionId=params["sessionId"])
        except Exception as e:
            log(f"ACK FAILED: {e}")
            self.cast_running = False   # watchdog restarts the session
            return
        # Rate cap screencast frames (WAN wedge lesson, ee723a7): busy
        # pages emit ~14fps/~1MB/s, outrunning the splice. Heartbeat and
        # boost frames are capped separately in _capture_once.
        now = time.monotonic()
        if now - self.last_frame_sent < MIN_FRAME_INTERVAL:
            return                      # dropped (acked, not sent)
        meta = params.get("metadata", {})
        if await self._broadcast_frame(params.get("data", ""),
                                       meta.get("scrollOffsetY", 0.0)):
            self.last_frame_sent = now

    async def _broadcast_tabs(self):
        await self._broadcast(json.dumps({
            "type": "tabs", "tabs": self.book.tabs(self.current_target)}))

    async def start_cast(self):
        if self.cast_running or not self.cdp:
            return
        await self.cdp.call("Page.startScreencast", format="jpeg",
                            quality=JPEG_QUALITY, maxWidth=VIEW_W,
                            maxHeight=VIEW_H, everyNthFrame=1)
        self.cast_running = True
        log("screencast started")

    async def stop_cast(self):
        if not self.cast_running or not self.cdp:
            return
        try:
            await self.cdp.call("Page.stopScreencast")
        except Exception:
            pass
        self.cast_running = False

    async def _watchdog(self):
        # Any frame source (screencast/heartbeat/boost) refreshes
        # last_frame_at; a total stall with clients attached means the
        # session or page socket is dead — restart, then re-attach.
        while True:
            await asyncio.sleep(2)
            if not self.clients or not self.cdp:
                continue
            if time.monotonic() - self.last_frame_at <= WATCHDOG_STALL:
                continue
            log("watchdog: no frames — restarting cast")
            self.last_frame_at = time.monotonic()  # one kick per interval
            self.last_frame_hash = ""
            try:
                await self.cdp.call("Page.stopScreencast")
            except Exception:
                pass
            self.cast_running = False
            try:
                await self.start_cast()
                await self._capture_once("watchdog")
            except Exception as e:
                log(f"watchdog restart failed: {e} — re-attaching")
                try:
                    async with self._retarget_lock:
                        await self.attach(self.current_target)
                        if self.clients:
                            await self.start_cast()
                except Exception as e2:
                    log(f"watchdog re-attach failed: {e2}")

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

    # ── Tabs ────────────────────────────────────────────────────────────
    async def _switch(self, target_id):
        async with self._retarget_lock:
            await self.stop_cast()
            await self.attach(target_id)
            if self.clients:
                await self.start_cast()
            # First-paint guarantee: never leave the canvas black while
            # waiting for the new target to damage.
            await self._capture_once("first-paint")

    # ── Input ───────────────────────────────────────────────────────────
    async def handle_input(self, msg):
        t = msg.get("type")
        if t == "ping":
            # App-level keepalive: intermediaries can silently kill an
            # idle client->bridge leg (protocol pings die ~10s idle while
            # data flows; app pings keep it up). The pong lets the page
            # detect a half-dead path and reconnect.
            await self._broadcast(json.dumps({"type": "pong"}))
            return
        c = self.cdp
        if not c:
            return
        if t == "click":
            x, y = msg["x"], msg["y"]
            self._move_pending = None   # don't interleave a stale move
            count = 2 if msg.get("dbl") else 1
            await c.call("Input.dispatchMouseEvent", type="mouseMoved",
                         x=x, y=y)
            for _ in range(count):
                await c.call("Input.dispatchMouseEvent",
                             type="mousePressed", x=x, y=y,
                             button="left", clickCount=count)
                await c.call("Input.dispatchMouseEvent",
                             type="mouseReleased", x=x, y=y,
                             button="left", clickCount=count)
            self._boost()
        elif t == "move":
            self._queue_move(msg["x"], msg["y"])
        elif t == "wheel":
            await c.call("Input.dispatchMouseEvent", type="mouseWheel",
                         x=msg.get("x", VIEW_W // 2),
                         y=msg.get("y", VIEW_H // 2),
                         deltaX=msg.get("dx", 0), deltaY=msg.get("dy", 0))
            self._boost()
        elif t == "text":
            await c.call("Input.insertText", text=msg["text"])
            self._boost()
        elif t == "key":
            name = msg["key"]
            if name in KEY_MAP:
                code, key, kcode = KEY_MAP[name]
                for kt in ("keyDown", "keyUp"):
                    await c.call("Input.dispatchKeyEvent", type=kt,
                                 windowsVirtualKeyCode=code, key=key,
                                 code=kcode)
            elif len(name) == 1:
                await c.call("Input.insertText", text=name)
            self._boost()
        elif t == "upload":
            ch = self.pending_chooser
            if not ch or ch.get("backendNodeId") is None:
                return
            paths = []
            for i, f in enumerate(msg.get("files", [])):
                name = "".join(c_ if c_.isalnum() or c_ in "._-"
                               else "_"
                               for c_ in f.get("name", f"file{i}"))[:80]
                path = f"/tmp/hive-upload-{i}-{name}"
                with open(path, "wb") as fh:
                    fh.write(base64.b64decode(f.get("data", "")))
                paths.append(path)
            if ch["mode"] == "selectSingle":
                paths = paths[:1]
            await c.call("DOM.setFileInputFiles", files=paths,
                         backendNodeId=ch["backendNodeId"])
            log(f"upload -> {paths}")
            self.pending_chooser = None
            await self._broadcast(json.dumps({"type": "upload_done"}))
            self._boost()
        elif t == "upload_cancel":
            ch = self.pending_chooser
            if ch and ch.get("backendNodeId") is not None:
                # Completing with an empty selection unblocks the page
                # (the input just stays empty, like pressing Cancel).
                try:
                    await c.call("DOM.setFileInputFiles", files=[],
                                 backendNodeId=ch["backendNodeId"])
                except Exception as e:
                    log(f"upload cancel: {e}")
            self.pending_chooser = None
            await self._broadcast(json.dumps({"type": "upload_done"}))
        elif t == "navigate":
            await c.call("Page.navigate", url=msg.get("url",
                                                      "about:blank"))
            self._boost()
        elif t == "newtab":
            r = await self.browser.call("Target.createTarget",
                                        url=msg.get("url", "about:blank"))
            await self._switch(r.get("targetId", ""))
        elif t == "switchtab":
            tid = msg.get("id", "")
            if tid and tid != self.current_target and \
                    tid in self.book.targets:
                log(f"switch -> {tid[:8]}")
                await self._switch(tid)
        elif t == "closetab":
            tid = msg.get("id", "")
            if tid and tid in self.book.targets:
                log(f"close {tid[:8]}")
                await self.browser.call("Target.closeTarget",
                                        targetId=tid)
                # If it was current, _on_browser_event's destroyed
                # handler re-attaches to the newest remaining tab.
        elif t == "tab":
            # v1 protocol: cycle prev/next. Kept so an old cast page
            # still works against this bridge.
            ids = [i for i in self.book.order if i in self.book.targets]
            if len(ids) < 2:
                return
            try:
                idx = ids.index(self.current_target)
            except ValueError:
                idx = 0
            dir_ = 1 if msg.get("dir", 1) >= 0 else -1
            await self._switch(ids[(idx + dir_) % len(ids)])
        elif t == "retarget":
            # v1 protocol: attach newest.
            await self._switch(self.book.newest())

    # ── WS client handling ──────────────────────────────────────────────
    async def client_loop(self, ws):
        self.clients.add(ws)
        log(f"client joined ({len(self.clients)} watching)")
        try:
            if not self.cdp:
                await self.attach()
            await self.start_cast()
            await self._broadcast_tabs()
            await self._capture_once("join")   # first paint, never black
            async for raw in ws:
                try:
                    await self.handle_input(json.loads(raw))
                except Exception as e:
                    log(f"input error: {e}")
                    await ws.send(json.dumps({"type": "error",
                                              "error": str(e)}))
        finally:
            self.clients.discard(ws)
            log(f"client left ({len(self.clients)} watching)")
            if not self.clients:
                await self.stop_cast()


def find_chrome():
    import glob
    import shutil
    for c in ("google-chrome", "google-chrome-stable", "chromium",
              "chromium-browser"):
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
    profile = args.profile or __import__("os").path.expanduser(
        "~/.browser-profile")

    bridge = Bridge(args.cdp_port, find_chrome(), profile)
    bridge.ensure_chrome()

    def process_request(connection, request):
        if request.path == "/status":
            return connection.respond(200, json.dumps({
                "ok": True, "clients": len(bridge.clients),
                "casting": bridge.cast_running, "cdp": args.cdp_port,
                "tabs": len(bridge.book.targets),
            }))
        return None

    asyncio.create_task(bridge._heartbeat())
    asyncio.create_task(bridge._watchdog())
    asyncio.create_task(bridge._keepalive())

    async with websockets.serve(bridge.client_loop, "127.0.0.1",
                                args.serve_port, max_size=64 * 1024 * 1024,
                                process_request=process_request):
        log(f"CDP :{args.cdp_port} -> ws://127.0.0.1:{args.serve_port}/cast")
        await asyncio.Future()          # run forever


if __name__ == "__main__":
    asyncio.run(main())
