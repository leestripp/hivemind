// hive-worker — Phase 30: remote container executor skeleton (30.2).
//
// Dials OUT to the hive-server worker channel and holds the connection:
//   hello (key auth) → welcome → heartbeat every 15s, receive loop for
//   server frames (exec arrives in 30.3), reconnect with backoff.
//
// Usage: hive-worker --server ws://host:8423 --key hmwk_... --name rig-gpu
//                    [--caps '{"gpu":true}']

#include "websocket/WebSocket.h"
#include "crypto/Crypto.h"
#include "net/Socket.h"
#include "OmniNet.h"
#include "runner.h"
#include "fleet.h"

#include <nlohmann/json.hpp>

#include <atomic>
#include <csignal>
#include <condition_variable>
#include <mutex>
#include <chrono>
#include <cstring>
#include <iostream>
#include <string>
#include <map>
#include <memory>
#include <thread>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <unordered_map>
#include <sstream>

namespace fs = std::filesystem;
using json = nlohmann::json;
using OmniNet::WebSocket::Connection;

// No custom SIGINT/SIGTERM handler on purpose: the default disposition
// kills the process, the OS closes the socket, and the server's receive
// loop immediately observes the disconnect (correct `online` flag).
// A flag-based graceful stop would hang in the blocking receiveText().
static std::atomic<bool> g_stop{false};

// Phase 33: in-progress skill syncs (id → state)
struct SkillSync {
    std::string agent;
    std::string skill;
    fs::path dir;
    int files = 0;
    int64_t bytes = 0;
};
static std::mutex sync_m;
static std::unordered_map<std::string, SkillSync> syncs;
// Phase H: in-flight workspace file pushes (id -> accumulated bytes)
static std::unordered_map<std::string, std::string> pushes;

// Phase B (v2): the fleet. Events from in-container hive-agent processes
// are forwarded to the server; set once the channel is up (see below).
static std::unique_ptr<hwfleet::Fleet> g_fleet;
static std::function<void(const json&)> g_send_frame;  // set per connection

int main(int argc, char* argv[]) {
    std::string server = "ws://127.0.0.1:8423";
    std::string key, name, caps = R"({"arch":"x86_64"})";
    for (int i = 1; i + 1 < argc; i += 2) {
        std::string a = argv[i];
        if (a == "--server") server = argv[i + 1];
        else if (a == "--key") key = argv[i + 1];
        else if (a == "--name") name = argv[i + 1];
        else if (a == "--caps") caps = argv[i + 1];
    }
    if (key.empty() || name.empty()) {
        std::cerr << "usage: hive-worker --server ws://host:8423 "
                     "--key hmwk_... --name <worker> [--caps '{...}']\n";
        return 2;
    }
    // Phase 35: advertise GPU capability. /dev/kfd is the AMD kernel
    // compute driver node — its presence means HIP workloads can run.
    // An explicit --caps gpu entry always wins.
    {
        auto c = json::parse(caps, nullptr, false);
        if (!c.is_object()) c = json::object();
        if (!c.contains("gpu") && fs::exists("/dev/kfd"))
            c["gpu"] = "amd";
        caps = c.dump();
    }
    const std::string url = server + "/api/workers/channel";

    // Phase B (v2): the fleet lives OUTSIDE the reconnect loop - agents
    // keep running across channel drops. Events route via g_send_frame,
    // which the connection below refreshes on every (re)connect.
    g_fleet = std::make_unique<hwfleet::Fleet>(
        [](const std::string& agent_id, const json& ev) {
            if (!g_send_frame) return;     // between connections: drop
            const std::string kind = ev.value("ev", "");
            json f;
            if (kind == "ready")
                f = {{"op","agent_status"},{"agent",agent_id},
                     {"state","ready"}};
            else if (kind == "delta")
                f = {{"op","task_delta"},{"agent",agent_id},
                     {"task",ev.value("task","")},
                     {"text",ev.value("text","")}};
            else if (kind == "message") {
                // Phase 35: agent → chat attachments. attach_file dropped
                // files into attachments/<tid>/outgoing/ in the workspace;
                // promote them to the canonical attachments/<tid>/ and
                // include records in task_result (hub adds "worker").
                json atts = json::array();
                const std::string tid = ev.value("thread_id", "");
                if (!tid.empty() && std::getenv("HOME")) {
                    const fs::path ws = fs::path{std::getenv("HOME")} /
                        ".local/share/hive-worker/workspace";
                    const fs::path outdir = ws / "attachments" / tid / "outgoing";
                    const fs::path canon  = ws / "attachments" / tid;
                    std::error_code ec;
                    if (fs::is_directory(outdir, ec)) {
                        for (const auto& e : fs::directory_iterator(outdir, ec)) {
                            if (!e.is_regular_file(ec)) continue;
                            const std::string fname =
                                e.path().filename().string();
                            fs::rename(e.path(), canon / fname, ec);
                            if (ec) { ec.clear(); continue; }
                            const auto sz = fs::file_size(canon / fname, ec);
                            atts.push_back({
                                {"name", fname},
                                {"rel_path", "attachments/" + tid + "/" + fname},
                                {"path", "/workspace/attachments/" + tid +
                                             "/" + fname},
                                {"size", ec ? 0 : (long long)sz},
                            });
                            ec.clear();
                        }
                        fs::remove(outdir, ec);   // emptied by the moves
                    }
                }
                f = {{"op","task_result"},{"agent",agent_id},
                     {"task",ev.value("task","")},
                     {"thread_id",tid},
                     {"body",ev.value("body","")},
                     {"error",ev.value("error","")},
                     {"attachments", std::move(atts)}};
            }
            else if (kind == "approval_request")
                f = {{"op","approval_req"},{"agent",agent_id},
                     {"id",ev.value("id","")},
                     {"command",ev.value("command","")}};
            else if (kind == "send_agent_message")
                f = {{"op","agent_msg"},{"agent",agent_id},
                     {"target",ev.value("target","")},
                     {"text",ev.value("text","")}};
            else if (kind == "status")
                f = {{"op","agent_status"},{"agent",agent_id},
                     {"state",ev.value("state","")},
                     {"detail",ev.value("error","")}};
            else
                f = {{"op","agent_log"},{"agent",agent_id},
                     {"event",ev}};
            try { g_send_frame(f); } catch (...) {}
        });

    // Phase 36: screen idle watcher — independent of the channel. Every
    // 60s: find hive-* containers with a live x11vnc, count established
    // VNC clients (TCP :5900), and after HIVEMIND_SCREEN_IDLE_MIN idle
    // minutes (default 15) tear the screen down via screen_ctl stop.
    std::thread([] {
        const char* env = std::getenv("HIVEMIND_SCREEN_IDLE_MIN");
        int idle_limit = (env && *env) ? std::atoi(env) : 15;
        if (idle_limit < 1) idle_limit = 15;
        std::unordered_map<std::string, int> idle_min;   // cname -> streak
        std::unordered_map<std::string, int> dns_fail;   // cname -> streak
        std::cerr << "worker: screen idle watcher up (limit "
                  << idle_limit << "min)\n";
        while (!g_stop) {
            std::this_thread::sleep_for(std::chrono::seconds(60));
            if (g_stop) break;
            std::string names;
            if (hwrunner::podman_simple({"ps", "--format", "{{.Names}}"},
                                        &names) != 0) {
                std::cerr << "worker: idle watcher podman ps failed\n";
                continue;
            }
            std::istringstream in(names);
            std::string cname;
            while (std::getline(in, cname)) {
                if (cname.rfind("hive-", 0) != 0) continue;

                // Phase 38: DNS wedge health check. Container DNS dies
                // when the pasta/aardvark plumbing wedges (observed on
                // Forge 2026-09-02: LLM calls then silently retry with
                // backoff). Probe getent (timeout-wrapped — a wedged
                // lookup can hang). After 3 consecutive failures,
                // `podman restart` re-creates the netns — but ONLY when
                // the agent is idle (state "ready" / not in the fleet):
                // a busy agent may be doing legit offline work (renders,
                // compiles). Busy agents get a loud warning; their
                // retrying LLM call eventually fails, the turn ends, and
                // the next tick recycles once idle.
                if (hwrunner::podman_simple(
                        {"exec", cname, "timeout", "8", "getent", "hosts",
                         "github.com"}, nullptr) == 0) {
                    dns_fail.erase(cname);
                } else {
                    const int streak = ++dns_fail[cname];
                    if (streak >= 3) {
                        const std::string aid = cname.substr(5);
                        std::string st;
                        if (g_fleet) {
                            auto sts = g_fleet->statuses();
                            st = sts.value(aid, "");
                        }
                        if (st.empty() || st == "ready") {
                            std::cerr << "worker: DNS wedged in " << cname
                                      << " (agent idle) — restarting"
                                         " container\n";
                            hwrunner::podman_simple({"restart", cname},
                                                    nullptr);
                            dns_fail.erase(cname);
                        } else if (streak == 3) {
                            std::cerr << "worker: DNS wedged in " << cname
                                      << " but agent is " << st
                                      << " — will recycle when idle\n";
                        }
                    }
                }

                // Screen state from /proc/net/tcp{,6} on :5900 (170C hex).
                // One exec, and zombie-proof: PID 1 (sleep) never reaps,
                // so pgrep matches defunct x11vnc forever — but zombies
                // hold NO sockets. No LISTEN ⇒ stack down; ESTABLISHED
                // conns = live viewers. (iproute2 isn't in the image.)
                std::string tcp;
                hwrunner::podman_simple({"exec", cname, "bash", "-c",
                                         "cat /proc/net/tcp /proc/net/tcp6"},
                                        &tcp);
                bool listening = false;
                int viewers = 0;
                std::istringstream ts(tcp);
                std::string line;
                while (std::getline(ts, line)) {
                    std::istringstream ls(line);
                    std::string sl, local, remote, st;
                    if (!(ls >> sl >> local >> remote >> st)) continue;
                    if (local.size() < 5 ||
                        local.substr(local.size() - 4) != "170C")
                        continue;
                    if (st == "0A") listening = true;         // LISTEN
                    else if (st == "01") viewers++;           // ESTABLISHED
                }
                if (!listening) { idle_min.erase(cname); continue; }
                if (viewers > 0) { idle_min[cname] = 0; continue; }
                const int streak = ++idle_min[cname];
                if (streak >= idle_limit) {
                    std::cerr << "worker: screen idle " << streak
                              << "min — stopping " << cname << "\n";
                    hwrunner::screen_ctl(cname.substr(5), "stop", "", "");
                    idle_min.erase(cname);
                }
            }
        }
    }).detach();

    int backoff = 1;
    while (!g_stop) {
        try {
            Connection ws{url};
            ws.sendText(json{{"op","hello"},{"worker",name},{"key",key},
                             {"caps",json::parse(caps)},{"proto",1},
                             {"models",hwfleet::list_worker_models()},
                             {"default_model",hwfleet::worker_default_model()}}.dump());

            // Handshake + protocol frames share ONE onText/readLoop path.
            // receiveText() for the welcome was a silent frame-eater: its
            // per-call buffer drops coalesced frames, and the hub's
            // reconcile agent_spawn rides in the SAME TLS record as the
            // welcome over wss/nginx — the spawn was eaten on every
            // reconnect (hit 2026-09-03: Forge never spawned until a
            // manual re-restart; the loopback droplet worker was immune
            // because its frames arrive in separate segments).
            std::mutex hs_m;
            std::condition_variable hs_cv;
            std::atomic<bool> welcomed{false};
            json welcome_j;

            // All sends (heartbeat thread + exec threads) share one mutex.
            std::mutex send_m;
            auto send_frame = [&](const json& f) {
                std::lock_guard lk(send_m);
                ws.sendText(f.dump());
            };
            g_send_frame = send_frame;
            std::atomic<bool> dead{false};
            auto connected_at = std::chrono::steady_clock::now();

            // ── 30.4: TCP streams over the channel ─────────────────────
            // stream_open → connect 127.0.0.1:port here, pump bytes both
            // ways as base64 stream_data frames; stream_close either side.
            // NOTE: no port allowlist — the channel is key-authenticated
            // and `exec` already grants the server a full shell here.
            struct Stream {
                std::unique_ptr<OmniNet::Socket> sock;
                std::atomic<bool> closed{false};
            };
            std::mutex streams_m;
            std::map<std::string, std::shared_ptr<Stream>> streams;
            auto close_stream = [&](const std::string& sid, bool tell_peer) {
                std::shared_ptr<Stream> st;
                {
                    std::lock_guard lk(streams_m);
                    auto it = streams.find(sid);
                    if (it == streams.end()) return;
                    st = it->second;
                    streams.erase(it);
                }
                if (st->closed.exchange(true)) return;   // already closed
                try { st->sock->close(); } catch (...) {}
                if (tell_peer) {
                    try { send_frame({{"op","stream_close"},{"sid",sid}}); }
                    catch (...) {}
                }
            };

            // Callback style + readLoop(): Connection::receiveText() uses a
            // per-call buffer and silently drops coalesced frames (exec
            // frames CAN arrive coalesced). readLoop() keeps its leftover.
            ws.onText([&](std::string msg) {
                json frame;
                try { frame = json::parse(msg); } catch (...) { return; }
                if (!welcomed.load()) {
                    // Handshake phase: welcome (or error) completes it; a
                    // reconcile frame coalesced behind welcome falls through
                    // to normal handling on the NEXT onText call.
                    const std::string op0 = frame.value("op", "");
                    if (op0 == "welcome" || op0 == "error") {
                        { std::lock_guard lk(hs_m); welcome_j = frame; }
                        welcomed = true;
                        hs_cv.notify_all();
                    }
                    return;
                }
                const std::string op = frame.value("op", "");
                if (op != "exec")
                    std::cout << "worker: frame: " << frame.dump() << "\n";

                // -- v2 fleet ops (Plan.md Phase B) --------------------
                if (op == "agent_spawn") {
                    // ensure_container may BUILD the image on first use:
                    // detached thread keeps the receive loop responsive.
                    const json specj = frame;
                    std::thread([specj] {
                        const std::string aid = specj.value("agent_id","");
                        try {
                            hwfleet::AgentSpec spec;
                            spec.id            = aid;
                            spec.name          = specj.value("name","");
                            spec.model         = specj.value("model","");
                            spec.api_host      = specj.value("api_host","");
                            spec.chat_endpoint = specj.value("chat_endpoint","");
                            spec.provider      = specj.value("provider","");
                            spec.max_context   = specj.value("max_context",0L);
                            spec.temperature   = specj.value("temperature",-1.0);
                            spec.auto_approve  = specj.value("auto_approve",false);
                            spec.system_prompt = specj.value("system_prompt","");
                            spec.mcp_servers   = specj.value("mcp_servers",
                                                             json::array());
                            spec.image         = specj.value("image","");
                            spec.type          = specj.value("type","");   // Phase 35
                            g_fleet->spawn(spec);
                        } catch (const std::exception& e) {
                            std::cerr << "worker: agent_spawn " << aid
                                      << " FAILED: " << e.what() << "\n";
                            try {
                                g_send_frame({{"op","agent_status"},
                                              {"agent",aid},
                                              {"state","down"},
                                              {"detail",e.what()}});
                            } catch (...) {}
                        }
                    }).detach();
                    return;
                }
                if (op == "agent_stop") {
                    const std::string aid = frame.value("agent_id","");
                    std::thread([aid] {
                        if (g_fleet) g_fleet->stop(aid);
                        try {
                            g_send_frame({{"op","agent_status"},
                                          {"agent",aid},{"state","down"}});
                        } catch (...) {}
                    }).detach();
                    return;
                }
                if (op == "task" || op == "approval_resp" || op == "cancel") {
                    const std::string aid = frame.value("agent_id","");
                    if (!g_fleet || !g_fleet->send_to(aid, frame)) {
                        try {
                            g_send_frame({{"op","task_result"},
                                          {"agent",aid},
                                          {"task",frame.value("id","")},
                                          {"thread_id",
                                           frame.value("thread_id","")},
                                          {"body",""},
                                          {"error","agent not running here"}});
                        } catch (...) {}
                    }
                    return;
                }

                if (op == "exec") {
                    std::cerr << "worker: exec " << frame.value("id","") << " received\n";
                    // 30.3: run in a detached thread — the receive loop
                    // stays responsive while chunks stream back.
                    const std::string id = frame.value("id", "");
                    const std::string agent = frame.value("agent", "");
                    std::vector<std::string> argv;
                    for (auto& a : frame.value("argv", json::array()))
                        argv.push_back(a.get<std::string>());
                    const std::string env = frame.value("env", "");
                    const int timeout = frame.value("timeout_s", 120);
                    std::thread([&, id, agent, argv, env, timeout] {
                        auto outcome = hwrunner::run_container_exec(
                            agent, argv, env, timeout,
                            [&](std::string_view chunk) {
                                try {
                                    send_frame({{"op","exec_chunk"},{"id",id},
                                        {"stream","stdout"},
                                        {"data",std::string{chunk}}});
                                    std::cerr << "worker: exec " << id << " chunk "
                                              << chunk.size() << "B: ["
                                              << std::string{chunk}.substr(0,60) << "]\n";
                                } catch (const std::exception& e) {
                                    std::cerr << "worker: exec " << id
                                              << " chunk SEND FAILED: " << e.what() << "\n";
                                }
                            });
                        json done{{"op","exec_done"},{"id",id},
                                  {"exit_code",outcome.exit_code},
                                  {"duration_ms",outcome.duration_ms}};
                        if (outcome.timed_out) done["timed_out"] = true;
                        if (!outcome.error.empty()) {
                            done["exit_code"] = -1;
                            try {
                                send_frame({{"op","exec_chunk"},{"id",id},
                                    {"stream","stderr"},
                                    {"data","[worker] " + outcome.error + "\n"}});
                            } catch (...) {}
                        }
                        try { send_frame(done); } catch (...) {}
                        std::cerr << "worker: exec " << id << " done, exit="
                                  << done["exit_code"].get<int>() << "\n";
                    }).detach();
                } else if (op == "agent_remove") {
                    // roster deleted: brain + container + local state gone.
                    const std::string aid = frame.value("agent_id", "");
                    try {
                        g_fleet->remove(aid);
                        std::cerr << "fleet: removed " << aid << "\n";
                    } catch (const std::exception& e) {
                        std::cerr << "fleet: remove " << aid
                                  << " failed: " << e.what() << "\n";
                    }
                } else if (op == "file_fetch") {
                    // Phase 31: read a file from THIS rig's shared
                    // workspace and stream it back as base64 chunks.
                    const std::string id = frame.value("id", "");
                    const std::string path = frame.value("path", "");
                    std::thread([&, id, path] {
                        auto fail = [&](const std::string& err) {
                            try { send_frame({{"op","file_error"},
                                              {"id",id},{"error",err}}); }
                            catch (...) {}
                        };
                        // Confine: path must be relative, no "..".
                        std::string rel = path;
                        while (!rel.empty() && rel.front() == '/')
                            rel.erase(rel.begin());
                        if (rel.rfind("workspace/", 0) == 0)
                            rel.erase(0, 10);   // strip agent-visible prefix
                        if (rel.empty() ||
                            rel.find("..") != std::string::npos) {
                            fail("bad path");
                            return;
                        }
                        const fs::path fpath =
                            fs::path{std::getenv("HOME")} /
                            ".local/share/hive-worker/workspace" / rel;
                        std::error_code ec;
                        if (!fs::is_regular_file(fpath, ec)) {
                            fail("not found: /workspace/" + rel);
                            return;
                        }
                        const auto size = fs::file_size(fpath, ec);
                        if (ec || size > 50u * 1024 * 1024) {
                            fail(ec ? "stat failed" : "file too large (50MB cap)");
                            return;
                        }
                        std::ifstream in(fpath, std::ios::binary);
                        if (!in) { fail("read failed"); return; }
                        std::string buf(192 * 1024, '\0');
                        while (in) {
                            in.read(buf.data(), (std::streamsize)buf.size());
                            const auto got = (size_t)in.gcount();
                            if (got == 0) break;
                            try {
                                send_frame({{"op","file_chunk"},{"id",id},
                                    {"data", OmniNet::Crypto::base64Encode(
                                        std::span<const uint8_t>{
                                            (const uint8_t*)buf.data(), got})}});
                            } catch (...) { return; }
                        }
                        try {
                            send_frame({{"op","file_done"},{"id",id},
                                        {"size",(int64_t)size}});
                        } catch (...) {}
                    }).detach();
                } else if (op == "file_list") {
                    // Phase H: list a dir in THIS rig's shared workspace.
                    const std::string id = frame.value("id", "");
                    const std::string path = frame.value("path", "");
                    auto fail = [&](const std::string& err) {
                        try { send_frame({{"op","file_error"},
                                          {"id",id},{"error",err}}); }
                        catch (...) {}
                    };
                    // Same confinement as file_fetch: relative, no "..".
                    std::string rel = path;
                    while (!rel.empty() && rel.front() == '/')
                        rel.erase(rel.begin());
                    if (rel.rfind("workspace/", 0) == 0)
                        rel.erase(0, 10);
                    if (rel.find("..") != std::string::npos) {
                        fail("bad path");
                        return;
                    }
                    const fs::path dpath =
                        fs::path{std::getenv("HOME")} /
                        ".local/share/hive-worker/workspace" / rel;
                    std::error_code ec;
                    if (!rel.empty() && !fs::is_directory(dpath, ec)) {
                        fail("not a dir: /workspace/" + rel);
                        return;
                    }
                    json entries = json::array();
                    for (auto& e : fs::directory_iterator(dpath, ec)) {
                        if (ec) break;
                        const std::string nm = e.path().filename().string();
                        if (!nm.empty() && nm.front() == '.') continue;
                        json j = {{"name", nm},
                                  {"path", rel.empty() ? nm
                                       : rel + "/" + nm},
                                  {"kind", e.is_directory(ec) ? "dir"
                                                              : "file"}};
                        if (!e.is_directory(ec)) {
                            std::error_code ec2;
                            j["size"] = (int64_t)e.file_size(ec2);
                        }
                        std::error_code ec3;
                        const auto mt = e.last_write_time(ec3);
                        if (!ec3) {
                            const auto sys = std::chrono::time_point_cast<
                                std::chrono::system_clock::duration>(
                                mt - fs::file_time_type::clock::now() +
                                std::chrono::system_clock::now());
                            j["mtime"] = (int64_t)
                                std::chrono::system_clock::to_time_t(sys);
                        }
                        entries.push_back(std::move(j));
                    }
                    try { send_frame({{"op","file_list_result"},
                                      {"id",id},{"entries",entries}}); }
                    catch (...) {}
                } else if (op == "file_push") {
                    // Phase H: push a file into THIS rig's shared
                    // workspace (server->worker). Chunked frames
                    // {id, path, first, eof, data:base64}; acked with
                    // file_push_ack on the eof frame (or on error).
                    const std::string id = frame.value("id", "");
                    auto fail = [&](const std::string& err) {
                        try { send_frame({{"op","file_push_ack"},{"id",id},
                                          {"ok",false},{"error",err}}); }
                        catch (...) {}
                        std::lock_guard lk(sync_m);
                        pushes.erase(id);
                    };
                    std::string rel = frame.value("path", "");
                    while (!rel.empty() && rel.front() == '/')
                        rel.erase(rel.begin());
                    if (rel.rfind("workspace/", 0) == 0)
                        rel.erase(0, 10);   // strip agent-visible prefix
                    if (rel.empty() ||
                        rel.find("..") != std::string::npos) {
                        fail("bad path");
                        return;
                    }
                    auto dec = OmniNet::Crypto::base64Decode(
                        frame.value("data", ""));
                    if (!dec) { fail("bad base64"); return; }
                    {
                        std::lock_guard lk(sync_m);
                        if (frame.value("first", false)) pushes[id] = {};
                        auto it = pushes.find(id);
                        if (it == pushes.end()) {   // mid-stream, unknown id
                            fail("unknown push id");
                            return;
                        }
                        it->second.append((const char*)dec->data(),
                                          dec->size());
                        if (it->second.size() > 50u * 1024 * 1024) {
                            fail("file too large (50MB cap)");
                            return;
                        }
                    }
                    if (frame.value("eof", false)) {
                        std::string data;
                        {
                            std::lock_guard lk(sync_m);
                            auto it = pushes.find(id);
                            if (it == pushes.end()) { fail("lost push"); return; }
                            data = std::move(it->second);
                            pushes.erase(it);
                        }
                        const fs::path fpath =
                            fs::path{std::getenv("HOME")} /
                            ".local/share/hive-worker/workspace" / rel;
                        std::error_code ec;
                        fs::create_directories(fpath.parent_path(), ec);
                        // World-writable: agent containers (rootless
                        // subuid mapping) must read/write alongside.
                        fs::permissions(fpath.parent_path(),
                                        fs::perms::all,
                                        fs::perm_options::replace, ec);
                        std::ofstream out(fpath, std::ios::binary |
                                                 std::ios::trunc);
                        out.write(data.data(), (std::streamsize)data.size());
                        out.close();
                        if (!out) { fail("write failed"); return; }
                        try {
                            send_frame({{"op","file_push_ack"},{"id",id},
                                        {"ok",true},
                                        {"bytes",(int64_t)data.size()}});
                        } catch (...) {}
                    }
                } else if (op == "skill_begin" || op == "skill_file" ||
                           op == "skill_end" || op == "skill_abort" ||
                           op == "skill_remove") {
                    // Phase 33: skill sync from the server. Files land in
                    // ~/.local/share/hive-worker/skills/<skill>/; on skill_end
                    // the agent's home gets ~/.agents/skills/<skill> →
                    // /skills/<skill> (absolute target resolves in-container).
                    const std::string id = frame.value("id", "");
                    auto fail = [&](const std::string& err) {
                        try { send_frame({{"op","skill_ack"},{"id",id},
                                          {"ok",false},{"error",err}}); }
                        catch (...) {}
                    };
                    const fs::path wbase = fs::path{std::getenv("HOME")} /
                                           ".local/share/hive-worker";
                    if (op == "skill_remove") {
                        const std::string agent = frame.value("agent", "");
                        const std::string skill = frame.value("skill", "");
                        std::error_code ec;
                        fs::remove(wbase / ("home-" + agent) /
                                   ".agents/skills" / skill, ec);
                        fs::remove_all(wbase / "skills" / skill, ec);
                        try { send_frame({{"op","skill_ack"},{"id",id},
                                          {"ok",!ec},
                                          {"error", ec ? ec.message() : ""}}); }
                        catch (...) {}
                    } else if (op == "skill_begin") {
                        const std::string skill = frame.value("skill", "");
                        // validate name: [a-z0-9-] only
                        bool okn = !skill.empty() && skill.size() <= 64;
                        for (char c : skill)
                            if (!((c>='a'&&c<='z')||(c>='0'&&c<='9')||c=='-'))
                                okn = false;
                        if (!okn) { fail("bad skill name"); }
                        else {
                            std::lock_guard lk(sync_m);
                            syncs[id] = SkillSync{frame.value("agent",""),
                                                  skill, {}, 0, 0};
                            std::error_code ec;
                            fs::remove_all(wbase / "skills" / skill, ec);
                            fs::create_directories(wbase / "skills" / skill, ec);
                        }
                    } else if (op == "skill_file") {
                        std::lock_guard lk(sync_m);
                        auto it = syncs.find(id);
                        if (it == syncs.end()) { fail("unknown sync id"); }
                        else {
                            SkillSync& sy = it->second;
                            std::string rel = frame.value("path", "");
                            // confine: relative, no ".."
                            while (!rel.empty() && rel.front() == '/')
                                rel.erase(rel.begin());
                            if (rel.empty() ||
                                rel.find("..") != std::string::npos) {
                                fail("bad path"); syncs.erase(it);
                            } else {
                                auto dec = OmniNet::Crypto::base64Decode(
                                    frame.value("data", ""));
                                if (!dec) { fail("bad base64"); syncs.erase(it); }
                                else {
                                    const fs::path fp =
                                        wbase / "skills" / sy.skill / rel;
                                    std::error_code ec;
                                    fs::create_directories(fp.parent_path(), ec);
                                    std::ofstream out(fp, std::ios::binary |
                                                          std::ios::app);
                                    out.write((const char*)dec->data(),
                                              (std::streamsize)dec->size());
                                    sy.bytes += (int64_t)dec->size();
                                    if (frame.value("eof", false)) sy.files++;
                                    if (sy.bytes > 50ll*1024*1024) {
                                        fail("skill too large");
                                        syncs.erase(it);
                                    }
                                }
                            }
                        }
                    } else if (op == "skill_abort") {
                        std::lock_guard lk(sync_m);
                        syncs.erase(id);   // partial files stay, harmless
                    } else {   // skill_end
                        SkillSync sy;
                        {
                            std::lock_guard lk(sync_m);
                            auto it = syncs.find(id);
                            if (it == syncs.end()) { fail("unknown sync id"); }
                            else { sy = it->second; syncs.erase(it); }
                        }
                        if (!sy.skill.empty()) {
                            // v2: brains read the canonical /skills mount directly
                            // (HIVEMIND_SKILLS_DIR) - no per-agent home symlinks.
                            try {
                                send_frame({{"op","skill_ack"},{"id",id},{"ok",true},
                                            {"files",sy.files},{"bytes",sy.bytes}});
                            } catch (...) {}
                            std::cerr << "[skills] synced " << sy.skill << " ("
                                      << sy.files << " files)\n";
                        }
                        }   // skill_end
                } else if (op == "model_ctl") {
                    // Hub → worker: set the default model (writes
                    // models.json "default" key). Synchronous reply.
                    const std::string id = frame.value("id", "");
                    const std::string model = frame.value("model", "");
                    json resp{{"op","model_result"},{"id",id}};
                    std::string err = hwfleet::set_default_model(model);
                    if (!err.empty()) resp["error"] = err;
                    else resp["default_model"] = model;
                    try { send_frame(resp); } catch (...) {}
                } else if (op == "screen_ctl") {
                    // 30.4: synchronous reply — the server blocks on it.
                    const std::string id = frame.value("id", "");
                    auto r = hwrunner::screen_ctl(
                        frame.value("agent", ""), frame.value("action", ""),
                        frame.value("type", ""),
                        frame.value("resolution", "1280x800"));
                    json resp{{"op","screen_result"},{"id",id},
                              {"running",r.running},{"port",r.port},
                              {"password",r.password}};
                    if (!r.error.empty()) resp["error"] = r.error;
                    try { send_frame(resp); } catch (...) {}
                } else if (op == "stream_open") {
                    const std::string sid = frame.value("sid", "");
                    const int port = frame.value("port", 0);
                    auto st = std::make_shared<Stream>();
                    try {
                        st->sock = OmniNet::createConnectedSocket(
                            "127.0.0.1", static_cast<uint16_t>(port),
                            std::chrono::milliseconds(3000));
                        if (!st->sock) throw std::runtime_error("connect failed");
                    } catch (const std::exception& e) {
                        try { send_frame({{"op","stream_error"},{"sid",sid},
                                          {"error",e.what()}}); } catch (...) {}
                        return;
                    }
                    {
                        std::lock_guard lk(streams_m);
                        streams[sid] = st;
                    }
                    try { send_frame({{"op","stream_opened"},{"sid",sid}}); }
                    catch (...) {}
                    // reader: socket → stream_data frames (detached; the
                    // shared_ptr keeps Stream alive until the pump exits)
                    std::thread([&, sid, st] {
                        for (;;) {
                            std::vector<uint8_t> chunk;
                            try { chunk = st->sock->recvAll(16384); }
                            catch (...) { break; }
                            if (chunk.empty()) break;   // EOF
                            try {
                                send_frame({{"op","stream_data"},{"sid",sid},
                                    {"b64", OmniNet::Crypto::base64Encode(
                                        std::span<const uint8_t>(chunk))}});
                            } catch (...) { break; }
                        }
                        close_stream(sid, /*tell_peer=*/true);
                    }).detach();
                } else if (op == "stream_data") {
                    const std::string sid = frame.value("sid", "");
                    std::shared_ptr<Stream> st;
                    {
                        std::lock_guard lk(streams_m);
                        auto it = streams.find(sid);
                        if (it != streams.end()) st = it->second;
                    }
                    if (st && !st->closed) {
                        auto bytes = OmniNet::Crypto::base64Decode(
                            frame.value("b64", ""));
                        if (bytes && !bytes->empty()) {
                            try {
                                st->sock->sendAll(std::span<const uint8_t>(
                                    *bytes));
                            } catch (...) { close_stream(sid, true); }
                        }
                    }
                } else if (op == "stream_close") {
                    close_stream(frame.value("sid", ""), /*tell_peer=*/false);
                }
            });

            // Pump reads on a thread so the main flow can await welcome;
            // on close it wakes the handshake waiter and ends hb below.
            std::thread pump([&] {
                try { ws.readLoop(); } catch (...) {}
                dead = true;
                hs_cv.notify_all();
            });

            json r;
            {
                std::unique_lock lk(hs_m);
                hs_cv.wait_for(lk, std::chrono::seconds(20),
                               [&] { return welcomed.load() || dead.load(); });
                r = welcome_j;
            }
            if (!welcomed.load() || r.value("op","") != "welcome") {
                std::cerr << "worker: rejected: "
                          << r.value("error", "no welcome") << "\n";
                try { ws.close(); } catch (...) {}
                pump.join();
                return 1;   // bad key / proto — reconnecting won't help
            }
            std::cout << "worker: connected as '" << name << "' (proto "
                      << r.value("proto", 0) << ")\n";
            backoff = 1;
            connected_at = std::chrono::steady_clock::now();

            // heartbeat sender; the pump thread drives reconnect via dead
            std::thread hb([&] {
                while (!g_stop && !dead) {
                    std::this_thread::sleep_for(std::chrono::seconds(15));
                    if (g_stop || dead) break;
                    try {
                        auto up = std::chrono::duration_cast<std::chrono::seconds>(
                            std::chrono::steady_clock::now() - connected_at).count();
                        send_frame({{"op","heartbeat"},{"uptime_s",up},
                                    {"agents", g_fleet
                                        ? g_fleet->statuses()
                                        : json::object()}});
                    } catch (...) { dead = true; }
                }
            });

            pump.join();   // blocks until the connection closes
            std::cerr << "worker: connection closed\n";
            if (hb.joinable()) hb.join();
        } catch (const std::exception& e) {
            std::cerr << "worker: exception: " << e.what() << "\n";
        }
        if (g_stop) break;
        std::cerr << "worker: reconnecting in " << backoff << "s\n";
        std::this_thread::sleep_for(std::chrono::seconds(backoff));
        backoff = std::min(backoff * 2, 30);
    }
    std::cout << "worker: stopped\n";
    return 0;
}
