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
#include "mcp_bridge.h"

#include <nlohmann/json.hpp>

#include <atomic>
#include <csignal>
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
    const std::string url = server + "/api/workers/channel";

    int backoff = 1;
    while (!g_stop) {
        try {
            Connection ws{url};
            ws.sendText(json{{"op","hello"},{"worker",name},{"key",key},
                             {"caps",json::parse(caps)},{"proto",1}}.dump());
            auto reply = ws.receiveText();
            json r = reply ? json::parse(*reply) : json{};
            if (r.value("op","") != "welcome") {
                std::cerr << "worker: rejected: "
                          << r.value("error", "no welcome") << "\n";
                return 1;   // bad key / proto — reconnecting won't help
            }
            std::cout << "worker: connected as '" << name << "' (proto "
                      << r.value("proto", 0) << ")\n";
            backoff = 1;
            auto connected_at = std::chrono::steady_clock::now();

            // All sends (heartbeat thread + exec threads) share one mutex.
            std::mutex send_m;
            auto send_frame = [&](const json& f) {
                std::lock_guard lk(send_m);
                ws.sendText(f.dump());
            };

            // heartbeat sender; receive loop below drives reconnect
            std::atomic<bool> dead{false};
            std::thread hb([&] {
                while (!g_stop && !dead) {
                    std::this_thread::sleep_for(std::chrono::seconds(15));
                    if (g_stop || dead) break;
                    try {
                        auto up = std::chrono::duration_cast<std::chrono::seconds>(
                            std::chrono::steady_clock::now() - connected_at).count();
                        send_frame({{"op","heartbeat"},{"uptime_s",up}});
                        hwbridge::reap_idle();
                    } catch (...) { dead = true; }
                }
            });

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
                const std::string op = frame.value("op", "");
                if (op != "exec")
                    std::cout << "worker: frame: " << frame.dump() << "\n";

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
                            // enable link in the agent's container home
                            std::error_code ec;
                            const fs::path linkdir = wbase /
                                ("home-" + sy.agent) / ".agents/skills";
                            fs::create_directories(linkdir, ec);
                            const fs::path link = linkdir / sy.skill;
                            fs::remove(link, ec);
                            fs::create_symlink(
                                fs::path("/skills") / sy.skill, link, ec);
                            if (ec) { fail("symlink: " + ec.message()); }
                            else {
                                try {
                                    send_frame({{"op","skill_ack"},
                                        {"id",id},{"ok",true},
                                        {"files",sy.files},
                                        {"bytes",sy.bytes}});
                                } catch (...) {}
                                std::cerr << "[skills] synced " << sy.skill
                                          << " for " << sy.agent << " ("
                                          << sy.files << " files)\n";
                            }
                        }
                    }
                } else if (op == "file_push") {
                    // Phase 33 D: server → worker file into the shared
                    // workspace. Chunks carry first/eof flags.
                    const std::string id = frame.value("id", "");
                    std::string rel = frame.value("path", "");
                    while (!rel.empty() && rel.front() == '/')
                        rel.erase(rel.begin());
                    if (rel.empty() || rel.find("..") != std::string::npos) {
                        try { send_frame({{"op","file_push_ack"},{"id",id},
                                          {"ok",false},{"error","bad path"}}); }
                        catch (...) {}
                    } else {
                        auto dec = OmniNet::Crypto::base64Decode(
                            frame.value("data", ""));
                        const fs::path fp =
                            fs::path{std::getenv("HOME")} /
                            ".local/share/hive-worker/workspace" / rel;
                        std::error_code ec;
                        fs::create_directories(fp.parent_path(), ec);
                        const bool first = frame.value("first", false);
                        std::ofstream out(fp, std::ios::binary |
                            (first ? std::ios::trunc : std::ios::app));
                        if (dec)
                            out.write((const char*)dec->data(),
                                      (std::streamsize)dec->size());
                        out.close();
                        if (frame.value("eof", false)) {
                            const auto sz = fs::file_size(fp, ec);
                            try { send_frame({{"op","file_push_ack"},
                                    {"id",id},{"ok",(bool)dec},
                                    {"bytes",(int64_t)(ec?0:sz)},
                                    {"error", dec ? "" : "bad base64"}}); }
                            catch (...) {}
                            std::cerr << "[files] pushed " << rel << " ("
                                      << (ec?0:sz) << " B)\n";
                        }
                    }
                } else if (op == "mcp_rpc") {
                    // Phase 33: tunnel a JSON-RPC call to an MCP server
                    // running inside the agent's container.
                    const std::string id = frame.value("id", "");
                    const std::string agent = frame.value("agent", "");
                    const std::string server = frame.value("server", "");
                    const std::string command = frame.value("command", "");
                    const std::vector<std::string> args =
                        frame.value("args", std::vector<std::string>{});
                    const json env = frame.value("env", json::object());
                    const std::string payload = frame.value("payload", "");
                    std::thread([=]() mutable {
                        std::string err;
                        std::string reply = hwbridge::rpc(
                            agent, server, command, args, env, payload,
                            120, err);
                        try {
                            if (reply.empty())
                                send_frame({{"op","mcp_rpc_reply"},{"id",id},
                                            {"ok",false},{"error",err}});
                            else
                                send_frame({{"op","mcp_rpc_reply"},{"id",id},
                                            {"ok",true},{"payload",reply}});
                        } catch (...) {}
                    }).detach();
                } else if (op == "screen_ctl") {
                    // 30.4: synchronous reply — the server blocks on it.
                    const std::string id = frame.value("id", "");
                    auto r = hwrunner::screen_ctl(
                        frame.value("agent", ""), frame.value("action", ""),
                        frame.value("image", ""),
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
            ws.readLoop();   // blocks until the connection closes
            std::cerr << "worker: connection closed\n";
            dead = true;
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
