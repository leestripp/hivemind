// include/hivemind/routes/ScreenRoutes.cpp — Phase 6 screen pipeline.
#include "hivemind/routes/ScreenRoutes.h"
#include "hivemind/routes/RouteUtil.h"
#include "hivemind/AgentStore.h"
#include "hivemind/AuthStore.h"
#include "hivemind/WorkerRouter.h"

#include <nlohmann/json.hpp>

#include <chrono>
#include <map>
#include <mutex>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

using json = nlohmann::json;
using OmniNet::HTTP::Request;
using OmniNet::HTTP::Response;
namespace routes = hivemind::routes;

namespace {

constexpr const char* kScreenCookie = "hive_screen";

// ── Auth for /screen/* (browser-facing: no Authorization header) ────────
// Accepts: Bearer header, ?key= query, or the hive_screen cookie set on
// first ?key= access. Requires admin or agent scope — read-only keys must
// not watch screens (they can show sensitive sessions).
bool screen_authorized(const Request& req, AuthStore& auth) {
    std::string token;
    if (auto hdr = req.header("Authorization")) {
        if (hdr->substr(0, 7) == "Bearer ") token = hdr->substr(7);
    }
    if (token.empty())
        token = req.queryParameter("key", "");
    if (token.empty()) {
        if (auto cookie = req.header("Cookie")) {
            const std::string needle = std::string{kScreenCookie} + "=";
            auto pos = cookie->find(needle);
            if (pos != std::string::npos) {
                auto end = cookie->find(';', pos);
                token = cookie->substr(pos + needle.size(),
                                       end == std::string::npos
                                           ? end : end - pos - needle.size());
            }
        }
    }
    if (token.empty()) return false;
    auto scopes = auth.validate(token);
    if (!scopes) return false;
    return AuthStore::has_scope(*scopes, "admin") ||
           AuthStore::has_scope(*scopes, "agent");
}

// Resolve :id from /api/agents/:id/screen[/start|stop]; empty on mismatch.
std::string agent_id_from_api(const Request& req) {
    auto seg = routes::path_segments(req.path());   // api agents :id screen ..
    if (seg.size() < 4 || seg[0] != "api" || seg[1] != "agents" ||
        seg[3] != "screen") return {};
    return seg[2];
}

// ── WS splice: raw TCP relay after both handshakes ──────────────────────
void pump(OmniNet::Socket& from, OmniNet::Socket& to) {
    std::vector<uint8_t> buf(64 * 1024);
    for (;;) {
        size_t n = 0;
        try { n = from.recv(buf); } catch (...) { break; }
        if (n == 0) break;
        try { to.sendAll(std::span<const uint8_t>(buf.data(), n)); }
        catch (...) { break; }
    }
}

// 30.4: the upstream is any connected Socket — a direct TCP socket for
// local containers, a ChannelSocket over the worker channel for remote.
void splice_to_container(std::unique_ptr<OmniNet::Socket> client,
                         std::unique_ptr<OmniNet::Socket> up,
                         uint16_t port, bool cast = false) {

    // Fresh upstream handshake: websockify (RFB, binary subprotocol) or
    // the cast bridge (JSON text frames, no subprotocol). Both legs speak
    // WS over the raw pipe, so the byte-transparent pump stays correct.
    const auto nonce = OmniNet::Crypto::randomBytes(16);
    const std::string key = OmniNet::Crypto::base64Encode(
        std::span<const uint8_t>(nonce.data(), nonce.size()));
    std::string handshake =
        std::string("GET ") + (cast ? "/cast" : "/websockify") +
        " HTTP/1.1\r\n"
        "Host: 127.0.0.1:" + std::to_string(port) + "\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Key: " + key + "\r\n"
        "Sec-WebSocket-Version: 13\r\n";
    if (!cast) handshake += "Sec-WebSocket-Protocol: binary\r\n";
    handshake += "\r\n";
    try {
        up->sendAll(std::span<const uint8_t>(
            reinterpret_cast<const uint8_t*>(handshake.data()), handshake.size()));
        std::string status_line;
        for (int i = 0; i < 100; ++i) {          // read head, cap lines
            auto line = up->readLine();
            if (!line) return;
            if (status_line.empty()) status_line = *line;
            if (*line == "\r\n" || line->empty()) break;
        }
        if (status_line.find(" 101") == std::string::npos) return;
    } catch (...) { return; }

    // Bidirectional pump; when either direction dies, close both.
    std::thread t([&] {
        pump(*client, *up);
        try { up->shutdown(SHUT_WR); } catch (...) {}
    });
    pump(*up, *client);
    try { client->shutdown(SHUT_WR); } catch (...) {}
    t.join();
}

std::unique_ptr<OmniNet::Socket> connect_upstream(const std::string& id,
                                                  uint16_t port) {
    // Remote agent? ChannelSocket over the worker channel (throws on
    // offline/open-failure — caller treats as "screen unavailable").
    if (auto remote = WorkerRouter::screen_stream(id, port)) return remote;
    try {
        return OmniNet::createConnectedSocket("127.0.0.1", port,
                                              std::chrono::milliseconds(3000));
    } catch (...) { return nullptr; }
}

/// HTTP GET over an arbitrary Socket (used for REMOTE noVNC assets —
/// the local path uses OmniNet's HTTP client). Connection: close gives
/// EOF-terminated bodies; returns {status, headers-skip-listed, body}.
bool http_fetch(OmniNet::Socket& sock, uint16_t port,
                const std::string& path, Response& resp) {
    const std::string req =
        "GET " + path + " HTTP/1.1\r\nHost: 127.0.0.1:" +
        std::to_string(port) + "\r\nConnection: close\r\n\r\n";
    try {
        sock.sendAll(std::span<const uint8_t>(
            reinterpret_cast<const uint8_t*>(req.data()), req.size()));
        std::string status_line;
        for (int i = 0; i < 100; ++i) {
            auto line = sock.readLine();
            if (!line) return false;
            if (status_line.empty()) {
                status_line = *line;   // "HTTP/1.1 200 OK"
                continue;
            }
            if (line->empty() || *line == "\r") break;
            auto colon = line->find(':');
            if (colon == std::string::npos) continue;
            std::string k = line->substr(0, colon);
            std::string v = line->substr(colon + 1);
            while (!v.empty() && v.front() == ' ') v.erase(v.begin());
            std::string lk = k;
            for (auto& c : lk) c = static_cast<char>(std::tolower(c));
            static const char* kSkip[] = {"connection", "keep-alive",
                "transfer-encoding", "content-length", "upgrade"};
            bool skip = false;
            for (auto* sk : kSkip) if (lk == sk) { skip = true; break; }
            if (!skip) resp.setHeader(k, v);
        }
        int status = 200;
        if (sscanf(status_line.c_str(), "HTTP/%*s %d", &status) != 1)
            status = 502;
        resp.setStatus(static_cast<OmniNet::HTTP::StatusCode>(status));
        std::string body;
        for (;;) {
            auto chunk = sock.recvAll(16384);
            if (chunk.empty()) break;
            body.append(reinterpret_cast<const char*>(chunk.data()),
                        chunk.size());
        }
        resp.setBody(body);
        return true;
    } catch (...) { return false; }
}

} // namespace

/// HiveScreen P0: self-contained cast viewer. Speaks the bridge's JSON
/// protocol over the same /screen/ws splice (kind=cast picks /cast
/// upstream). Auth rides the ?key= query the share link already carries.
static const char kCastPage[] = R"CAST(<!doctype html>
<html><head><meta charset=utf-8><title>HiveScreen</title>
<meta name=viewport content="width=device-width,initial-scale=1">
<style>
:root{--bg:#14161a;--panel:#1d2026;--border:#333;--fg:#e8e8ea;--accent:#2b6cb0}
*{box-sizing:border-box}
body{margin:0;background:var(--bg);color:var(--fg);font:16px system-ui,sans-serif;
     height:100vh;display:grid;grid-template-rows:auto auto 1fr}
#bar{display:flex;gap:8px;padding:8px;background:var(--panel);align-items:center}
#url{flex:1;min-width:100px;background:#0f1114;border:1px solid var(--border);
     color:#eee;padding:9px 10px;border-radius:6px;font-size:16px}
button{background:var(--accent);border:0;color:#fff;padding:9px 14px;border-radius:6px;
       cursor:pointer;font-size:16px;white-space:nowrap}
button:hover{background:#3182ce}
#zoom{background:#3a3f47}
/* Fixed-width status: text changes never reflow the bar (v1's "moving
   buttons" bug). */
#stat{margin-left:auto;min-width:230px;text-align:right;
      font:14px/1.3 ui-monospace,monospace;color:#9aa}
#dot{display:inline-block;width:10px;height:10px;border-radius:50%;
     background:#666;margin-right:6px;vertical-align:middle}
#tabs{display:flex;gap:6px;padding:6px 8px;background:#191c21;overflow-x:auto;
      min-height:48px;align-items:center;border-bottom:1px solid var(--border)}
.tab{display:flex;align-items:center;gap:6px;background:#23262d;
     border:1px solid var(--border);color:#ccc;padding:8px 12px;border-radius:6px;
     max-width:270px;cursor:pointer;flex:0 0 auto;font-size:15px}
.tab.active{background:var(--accent);color:#fff;border-color:#3182ce}
.tab .t{overflow:hidden;text-overflow:ellipsis;white-space:nowrap;max-width:200px}
.tab .x{background:transparent;border:0;padding:0 4px;color:inherit;font-size:16px;
        border-radius:4px;cursor:pointer}
.tab .x:hover{background:rgba(255,255,255,.25)}
#newtab{flex:0 0 auto;background:#3a3f47;font-size:18px;padding:6px 14px}
#view{overflow:auto;display:flex;justify-content:center;padding:10px}
canvas{background:#000;cursor:crosshair}
#view.fit canvas{max-width:calc(100vw - 20px);height:auto}
#fcover{display:none;position:fixed;inset:0;background:rgba(0,0,0,.75);z-index:9}
#fcbox{max-width:420px;margin:15vh auto;background:var(--panel);
       border:1px solid var(--border);border-radius:8px;padding:18px;font-size:16px}
#fcbox input{color:#eee;margin-top:10px;font-size:15px}
#fcbox button{margin-top:12px}
</style></head><body>
<div id=bar>
<input id=url placeholder="address — type a URL, press Enter">
<button onclick="go()">Go</button>
<button id=zoom title="toggle zoom">Zoom: Fit</button>
<span id=stat><span id=dot></span><span id=stxt>connecting…</span></span>
</div>
<div id=tabs></div>
<div id=view class=fit><canvas id=cv width=1280 height=800></canvas></div>
<div id=fcover>
 <div id=fcbox>
  <p>The page is asking for a file.<br>Pick it here and it lands straight in that browser.</p>
  <input type=file id=fcfile><br>
  <button id=fcgo>Upload</button> <button id=fccancel>Cancel</button>
 </div>
</div>
<script>
const q=new URLSearchParams(location.search);
const agent=q.get('agent')||location.pathname.split('/')[2];
const key=q.get('key')||'';
const proto=location.protocol==='https:'?'wss':'ws';
const cv=document.getElementById('cv'),ctx=cv.getContext('2d');
const dot=document.getElementById('dot'),stxt=document.getElementById('stxt');
const tabsEl=document.getElementById('tabs'),view=document.getElementById('view');
const img=new Image();
img.onload=()=>{ctx.drawImage(img,0,0);};
let frames=0,lastFrame=0,lastRx=Date.now(),ws=null,retry=0;
let fpsWin=[];   // frame arrival timestamps (rolling 2s)

// Zoom: Fit scales the 1280x800 frame to the window; 100% is native
// pixels with scrollbars. Persisted — lee is visually impaired.
let zoom=localStorage.getItem('hs-zoom')||'fit';
function applyZoom(){view.className=zoom;
 document.getElementById('zoom').textContent='Zoom: '+(zoom==='fit'?'Fit':'100%');}
document.getElementById('zoom').onclick=()=>{zoom=zoom==='fit'?'full':'fit';
 localStorage.setItem('hs-zoom',zoom);applyZoom();};
applyZoom();

function renderTabs(tabs){
 tabsEl.innerHTML='';
 for(const t of tabs){
  const b=document.createElement('button');
  b.className='tab'+(t.active?' active':'');
  b.title=t.title+'\n'+t.url;
  const span=document.createElement('span');span.className='t';span.textContent=t.title;
  b.appendChild(span);
  const x=document.createElement('button');x.className='x';x.textContent='\u00d7';
  x.title='close tab';
  x.onclick=e=>{e.stopPropagation();send({type:'closetab',id:t.id});};
  b.appendChild(x);
  b.onclick=()=>send({type:'switchtab',id:t.id});
  tabsEl.appendChild(b);
 }
 const plus=document.createElement('button');
 plus.id='newtab';plus.textContent='+';plus.title='new tab';
 plus.onclick=()=>send({type:'newtab',url:'about:blank'});
 tabsEl.appendChild(plus);
}

function connect(){
 ws=new WebSocket(`${proto}://${location.host}/screen/ws?agent=${agent}&key=${encodeURIComponent(key)}`);
 ws.onopen=()=>{retry=0;};
 ws.onclose=()=>{retry=Math.min(retry+1,5);stxt.textContent=`reconnecting (${retry})…`;
  dot.style.background='#c53030';setTimeout(connect,1000*retry);};
 ws.onerror=()=>{try{ws.close();}catch(_){}};
 ws.onmessage=e=>{lastRx=Date.now();try{const m=JSON.parse(e.data);
  if(m.type==='frame'){img.src='data:image/jpeg;base64,'+m.data;
   frames++;lastFrame=Date.now();fpsWin.push(lastFrame);}
  else if(m.type==='tabs'){renderTabs(m.tabs);}
  else if(m.type==='filechooser'){fcShow(m.multiple);}
  else if(m.type==='upload_done'){fcHide();}
  else if(m.type==='pong'){}
  else if(m.type==='error'){stxt.textContent='bridge: '+m.error;}}catch(_){}};
}
connect();

// Liveness: honest indicator driven by actual frame arrivals.
// LIVE <2s, STALE 2-10s, FROZEN >10s (force reconnect).
setInterval(()=>{
 const now=Date.now(),age=lastFrame?(now-lastFrame)/1000:Infinity;
 fpsWin=fpsWin.filter(t=>now-t<2000);
 const fps=(fpsWin.length/2).toFixed(1);
 if(!ws||ws.readyState!==1){return;}
 if(age<2){dot.style.background='#38a169';stxt.textContent=`LIVE — ${fps} fps — ${frames} frames`;}
 else if(age<10){dot.style.background='#d69e2e';stxt.textContent=`STALE — ${age.toFixed(0)}s — ${frames} frames`;}
 else{dot.style.background='#c53030';stxt.textContent=`FROZEN — ${frames} frames`;}
},1000);

// Client keepalive: an idle client->bridge leg dies silently after ~10s
// on this path (protocol pings don't save it; app messages do) — ping
// every 5s; if NOTHING arrived for 15s, force a reconnect.
setInterval(()=>{send({type:'ping'});
 if(Date.now()-lastRx>15000){try{ws.close();}catch(_){}}},5000);
function send(o){if(ws&&ws.readyState===1)ws.send(JSON.stringify(o));}

// File-chooser relay: headless Chrome can't show a native picker, so the
// bridge intercepts it and asks us; we ship the file back as base64 and
// the bridge calls DOM.setFileInputFiles.
const fcover=document.getElementById('fcover'),fcfile=document.getElementById('fcfile');
function fcShow(mult){fcfile.multiple=!!mult;fcfile.value='';fcover.style.display='block';}
function fcHide(){fcover.style.display='none';}
document.getElementById('fccancel').onclick=()=>{send({type:'upload_cancel'});fcHide();};
document.getElementById('fcgo').onclick=async()=>{
 if(!fcfile.files.length){fcfile.click();return;}
 stxt.textContent='uploading…';
 const files=[];
 for(const f of fcfile.files){
  const b64=await new Promise(res=>{const r=new FileReader();
   r.onload=()=>res(r.result.split(',')[1]);r.readAsDataURL(f);});
  files.push({name:f.name,data:b64});
 }
 send({type:'upload',files});
};
function go(){const u=document.getElementById('url').value.trim();
 if(u)send({type:'navigate',url:u.includes('://')?u:'https://'+u});}
document.getElementById('url').addEventListener('keydown',e=>{if(e.key==='Enter')go();e.stopPropagation();});

// coords: canvas CSS size -> 1280x800 frame space
function pos(ev){const r=cv.getBoundingClientRect();
 return {x:Math.round((ev.clientX-r.left)*1280/r.width),y:Math.round((ev.clientY-r.top)*800/r.height)};}
cv.addEventListener('click',e=>send({type:'click',...pos(e)}));
cv.addEventListener('dblclick',e=>send({type:'click',dbl:true,...pos(e)}));
cv.addEventListener('contextmenu',e=>e.preventDefault());
let lastMove=0;
cv.addEventListener('mousemove',e=>{const n=Date.now();if(n-lastMove>80){lastMove=n;send({type:'move',...pos(e)});}});
cv.addEventListener('wheel',e=>{e.preventDefault();send({type:'wheel',...pos(e),dx:e.deltaX,dy:e.deltaY});},{passive:false});
const SPECIAL={Enter:1,Backspace:1,Tab:1,Escape:1,Delete:1,Home:1,End:1,PageUp:1,PageDown:1,
 ArrowLeft:1,ArrowRight:1,ArrowUp:1,ArrowDown:1};
document.addEventListener('keydown',e=>{
 if(e.target.id==='url')return;
 if(SPECIAL[e.key]){e.preventDefault();send({type:'key',key:e.key});}
 else if(e.key.length===1&&!e.ctrlKey&&!e.metaKey&&!e.altKey){e.preventDefault();send({type:'text',text:e.key});}});
</script></body></html>
)CAST";

void register_screen_routes(OmniNet::HTTP::Server& server,
                            AgentStore& agents,
                            AuthStore& auth,
                            std::function<uint16_t(const std::string&)>
                                port_resolver) {
    // Resolve an agent's screen port: test override, else podman.
    // NOTE: capture BY VALUE — route handlers outlive this function.
    auto resolve = [port_resolver](const std::string& id) -> uint16_t {
        if (port_resolver) return port_resolver(id);
        // 30.4: worker-assigned agent → screen_ctl status over channel.
        try {
            if (auto r = WorkerRouter::screen_ctl(id, "status"))
                return (r->error.empty() && r->running) ? r->port
                                                        : uint16_t{0};
        } catch (...) { return 0; }   // worker offline → screen down
        return 0;   // v2: no worker -> no screen
    };
    // 30.4: remote screen password (rides in the redirect URL).
    auto password_of = [](const std::string& id) -> std::string {
        try {
            if (auto r = WorkerRouter::screen_ctl(id, "status"))
                return r->password;
        } catch (...) {}
        return {};   // v2: no worker -> no password
    };
    // HiveScreen P0: "cast" (CDP bridge, JSON text WS) vs "rfb" (noVNC).
    auto kind_of = [](const std::string& id) -> std::string {
        try {
            if (auto r = WorkerRouter::screen_ctl(id, "status"))
                return r->kind.empty() ? "rfb" : r->kind;
        } catch (...) {}
        return "rfb";
    };
    // ── GET /api/agents/:id/screen — status ─────────────────────────────
    server.get("/api/agents/:id/screen",
        [&agents, resolve](const Request& req, Response& resp) {
            const std::string id = agent_id_from_api(req);
            if (id.empty() || !agents.get_agent(id)) {
                routes::send_error(resp, 404, "agent not found"); return;
            }
            const uint16_t port = resolve(id);
            // v2: podman/image facts live on the WORKER - the channel is
            // the only source of truth; no worker -> screen stack down.
            // screen_ctl THROWS when the worker is offline — report 503
            // (start handler does), not an uncaught 500.
            std::optional<WorkerChannel::ScreenResult> remote;
            try { remote = WorkerRouter::screen_ctl(id, "status"); }
            catch (const std::exception& e) {
                routes::send_error(resp, 503, e.what()); return;
            }
            routes::send_json(resp, json{
                {"podman", remote.has_value()},
                {"browser_image", remote.has_value() && remote->error.empty()},
                {"running", port != 0},
                {"port", port},
                {"kind", remote && !remote->kind.empty() ? remote->kind
                                                         : "rfb"},
                {"url", port ? "/screen/" + id + "/" : ""},
            });
        });

    // ── POST /api/agents/:id/screen/start ───────────────────────────────
    server.post("/api/agents/:id/screen/start",
        [&agents](const Request& req, Response& resp) {
            const std::string id = agent_id_from_api(req);
            const auto agent_rec = id.empty() ? std::nullopt
                                              : agents.get_agent(id);
            if (id.empty() || !agent_rec) {
                routes::send_error(resp, 404, "agent not found"); return;
            }
            // Phase 72.2: host agents DO have a screen — each hivex
            // user gets its own Xvfb slot on the worker host (display
            // :99+slot, websockify 6080+slot); the worker's
            // screen_ctl_host drives it. No hub-side gate.
            (void)agent_rec;
            try {
                // 30.4: worker-assigned → screen_ctl start over channel.
                if (auto r = WorkerRouter::screen_ctl(id, "start")) {
                    if (!r->error.empty()) {
                        routes::send_error(resp, 502, r->error); return;
                    }
                    routes::send_json(resp, json{{"running", r->running},
                                                 {"port", r->port},
                                                 {"url", "/screen/" + id + "/"}});
                    return;
                }
                routes::send_error(resp, 503, "agent's worker is offline");
            } catch (const std::exception& e) {
                routes::send_error(resp, 502, e.what());
            }
        });

    // ── POST /api/agents/:id/screen/stop ────────────────────────────────
    server.post("/api/agents/:id/screen/stop",
        [&agents](const Request& req, Response& resp) {
            const std::string id = agent_id_from_api(req);
            if (id.empty() || !agents.get_agent(id)) {
                routes::send_error(resp, 404, "agent not found"); return;
            }
            // 30.4: worker-assigned → screen_ctl stop over channel.
            try {
                if (WorkerRouter::screen_ctl(id, "stop")) {
                    routes::send_json(resp, json{{"running", false},
                                                 {"stopped", true}});
                    return;
                }
            } catch (const std::exception& e) {
                routes::send_error(resp, 502, e.what()); return;
            }
            routes::send_error(resp, 503, "agent's worker is offline");
        });

    // ── GET /screen/* — noVNC asset proxy (own auth) ────────────────────
    server.get("/screen/*",
        [&agents, &auth, resolve, password_of, kind_of](const Request& req, Response& resp) {
            if (!screen_authorized(req, auth)) {
                routes::send_error(resp, 401, "missing or invalid key"); return;
            }
            auto seg = routes::path_segments(req.path());  // screen :id rest…
            if (seg.size() < 2 || !agents.get_agent(seg[1])) {
                routes::send_error(resp, 404, "agent not found"); return;
            }
            const uint16_t port = resolve(seg[1]);
            if (port == 0) {
                // Phase 66f: SELF-HEALING share link. A parked/stopped
                // screen used to dead-end here with a bare 503 — the
                // recurring "forgot noVNC again" report. Instead: kick
                // an async start on the worker (pin + maybe recreate +
                // Xvfb/x11vnc/websockify + pasta grace = 10-45s) and
                // serve an auto-refresh page; the next load lands on
                // live noVNC. A 60s in-flight window dedups refreshes
                // so concurrent starts can't stack (double websockify).
                const std::string id = seg[1];
                static std::mutex starts_m;
                static std::map<std::string, std::chrono::steady_clock::time_point>
                    starts;
                bool kick = false;
                {
                    std::lock_guard<std::mutex> lk(starts_m);
                    auto it = starts.find(id);
                    const auto now = std::chrono::steady_clock::now();
                    if (it == starts.end() ||
                        now - it->second > std::chrono::seconds(60)) {
                        starts[id] = now;
                        kick = true;
                    }
                }
                if (kick)
                    std::thread([id] {
                        try { WorkerRouter::screen_ctl(id, "start"); }
                        catch (...) {}
                    }).detach();
                resp = OmniNet::HTTP::Response::ok();
                resp.setContentType("text/html; charset=utf-8");
                resp.setHeader("Cache-Control",
                               "no-cache, no-store, must-revalidate");
                resp.setHeader("Refresh", "8");
                routes::apply_security_headers(resp);
                resp.setBody(
                    "<!doctype html><meta charset=utf-8>"
                    "<title>Starting screen\u2026</title>"
                    "<body style='font-family:sans-serif;background:#111;"
                    "color:#eee;display:flex;align-items:center;"
                    "justify-content:center;height:100vh;margin:0'>"
                    "<div style='text-align:center'>"
                    "<h2>Screen session is starting\u2026</h2>"
                    "<p>The desktop stack was parked. It is being woken "
                    "now \u2014 this page refreshes automatically "
                    "(usually 10\u201340 seconds).</p></div>");
                return;
            }

            // HiveScreen P0: cast agents skip noVNC entirely — one
            // self-contained page, no asset proxying, no cookie (the
            // page passes ?key= straight into its WS URL). The page's
            // WS rides the same /screen/ws splice with kind=cast.
            if (kind_of(seg[1]) == "cast") {
                resp = OmniNet::HTTP::Response::ok();
                resp.setContentType("text/html; charset=utf-8");
                resp.setHeader("Cache-Control",
                               "no-cache, no-store, must-revalidate");
                resp.setBody(kCastPage);
                routes::apply_security_headers(resp);
                return;
            }

            // First ?key= access: set the cookie so relative asset loads
            // (and the WS handshake) authenticate without query params.
            // Deferred until resp is final — Response::ok() reassignment
            // wipes headers set beforehand (Phase 9 CORS bug class).
            std::string set_cookie;
            if (!req.queryParameter("key", "").empty())
                set_cookie = std::string{kScreenCookie} + "=" +
                             req.queryParameter("key", "") +
                             "; Path=/screen; HttpOnly; SameSite=Strict";

            // Root: redirect into noVNC with autoconnect. The key rides
            // along in the Location (clients without a cookie jar — e.g.
            // OmniNet's — follow without the cookie we just set).
            if (seg.size() == 2) {
                // Absolute Location — OmniNet's client won't follow relative
                // redirects; browsers accept either. Scheme: behind TLS
                // nginx the hardcoded http:// downgraded the noVNC page to
                // plaintext (and its WS to ws://) — honour X-Forwarded-Proto,
                // default https (the site is TLS-only).
                std::string scheme = "https";
                if (auto xfp = req.header("X-Forwarded-Proto"))
                    if (!xfp->empty()) scheme = *xfp;
                std::string loc = scheme + "://" +
                    std::string{req.header("Host").value_or("localhost")} +
                    "/screen/" + seg[1] +
                    "/vnc.html?autoconnect=true&resize=scale"
                    "&path=%2Fscreen%2Fws%3Fagent%3D" + seg[1] +   // ABSOLUTE — relative paths resolve against /screen/<id>/vnc.html and double up
                    "&password=" + password_of(seg[1]);
                if (!req.queryParameter("key", "").empty())
                    loc += "&key=" + req.queryParameter("key", "");
                if (req.queryParameter("view_only", "") == "1")
                    loc += "&view_only=true";   // Phase 28: watch, don't touch
                resp.setStatus(static_cast<OmniNet::HTTP::StatusCode>(302));
                resp.setHeader("Location", loc);
                if (!set_cookie.empty()) resp.setHeader("Set-Cookie", set_cookie);
                return;
            }

            // Asset proxy: /screen/<id>/<rest> → http://127.0.0.1:port/<rest>
            std::string rest;
            for (size_t i = 2; i < seg.size(); ++i) {
                if (seg[i] == "..") { routes::send_error(resp, 400, "bad path"); return; }
                rest += "/" + seg[i];
            }

            // 30.4: remote agent → fetch over the worker channel.
            {
                std::unique_ptr<OmniNet::Socket> remote;
                try { remote = WorkerRouter::screen_stream(seg[1], port); }
                catch (const std::exception& e) {
                    routes::send_error(resp, 503, e.what()); return;
                }
                if (remote) {
                    resp = OmniNet::HTTP::Response::ok();
                    if (!http_fetch(*remote, port, rest, resp)) {
                        routes::send_error(resp, 502,
                                           "remote screen asset fetch failed");
                        return;
                    }
                    if (!set_cookie.empty())
                        resp.setHeader("Set-Cookie", set_cookie);
                    return;
                }
            }

            OmniNet::HTTP::ClientRequest creq;
            creq.method("GET")
                .url("http://127.0.0.1:" + std::to_string(port) + rest);
            auto res = OmniNet::HTTP::Client::get_instance().send_blocking(creq);
            if (!res) { routes::send_error(resp, 502, "upstream unreachable"); return; }

            resp = OmniNet::HTTP::Response::ok();
            resp.setStatus(res->status());
            static const char* kSkip[] = {"connection", "keep-alive",
                "transfer-encoding", "content-length", "upgrade"};
            for (auto& [k, v] : res->headers()) {
                std::string lk = k;
                for (auto& c : lk) c = static_cast<char>(std::tolower(c));
                bool skip = false;
                for (auto* s : kSkip) if (lk == s) { skip = true; break; }
                if (!skip) resp.setHeader(k, v);
            }
            resp.setBody(res->body());
            if (!set_cookie.empty()) resp.setHeader("Set-Cookie", set_cookie);
        });

    // ── WS /screen/ws?agent=<id> — splice to container websockify ────────
    OmniNet::HTTP::Server::ProtocolSwitchHandlers h;
    h.on_upgrade = [&agents, &auth, resolve](const Request& req, Response& resp) {
        const std::string id = req.queryParameter("agent", "");
        const uint16_t port = id.empty() ? 0 : resolve(id);
        if (!screen_authorized(req, auth) || id.empty() ||
            !agents.get_agent(id) || port == 0) {
            routes::send_error(resp, 403, "screen unavailable"); return;
        }
        auto wskey = req.header("Sec-WebSocket-Key");
        if (!wskey) { routes::send_error(resp, 400, "not a websocket request"); return; }
        resp.setStatus(static_cast<OmniNet::HTTP::StatusCode>(101));
        resp.setHeader("Upgrade", "websocket");
        resp.setHeader("Connection", "Upgrade");
        resp.setHeader("Sec-WebSocket-Accept",
                       OmniNet::WebSocket::Handshake::computeAcceptKey(std::string{*wskey}));
        if (auto proto = req.header("Sec-WebSocket-Protocol"))
            if (proto->find("binary") != std::string::npos)
                resp.setHeader("Sec-WebSocket-Protocol", "binary");
        resp.markProtocolSwitch();
    };
    h.on_post_upgrade = [resolve, kind_of](
                           std::unique_ptr<OmniNet::Socket> socket,
                           const Request& req) {
        const std::string id = req.queryParameter("agent", "");
        const uint16_t port = resolve(id);
        if (port == 0) return;
        const bool cast = kind_of(id) == "cast";
        std::thread([client = std::move(socket), id, port, cast]() mutable {
            std::unique_ptr<OmniNet::Socket> up;
            try { up = connect_upstream(id, port); }
            catch (...) { return; }   // client socket closes with `client`
            if (!up) return;
            splice_to_container(std::move(client), std::move(up), port, cast);
        }).detach();
    };
    server.registerProtocolSwitch("/screen/ws", std::move(h));
}
