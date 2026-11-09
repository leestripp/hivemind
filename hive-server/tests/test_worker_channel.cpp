// Regression test for the dev-rig incident (2026-08-30): an abruptly-killed
// worker must not wedge the channel. Two hazards were fixed in
// WorkerChannel:
//   1. send() held m_ across a BLOCKING sendText -- a dead-but-undetected
//      peer (TCP retransmit stall) froze the mutex, wedging every other
//      worker's connect/send behind it. send() now only enqueues; a
//      per-connection writer thread does the blocking writes.
//   2. disconnect cleanup erased conns_[name] unconditionally, which could
//      wipe a NEWER replacement conn for the same worker name.
// This test: connect worker "rig1", kill it, immediately reconnect under
// the same name, then prove the channel is functional (online + a frame
// sent via send() arrives at the new connection).

#include "hivemind/WorkerChannel.h"
#include "hivemind/HostStats.h"
#include "hivemind/WorkerStore.h"
#include "websocket/WebSocket.h"
#include "crypto/Crypto.h"

#include <nlohmann/json.hpp>

#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <thread>

using nlohmann::json;
using OmniNet::WebSocket::Connection;

static const char* kPort = "18491";
static const std::string kUrl =
    std::string("ws://127.0.0.1:") + kPort + "/api/workers/channel";

static json hello(const std::string& key) {
    // Legacy catalog fields remain harmless compatibility input. Current
    // model configuration is hub-owned and never negotiated per worker.
    return json{{"op", "hello"},   {"worker", "rig1"}, {"key", key},
                {"caps", json::object()}, {"proto", 1},
                {"models", json::array({json{{"name", "legacy-model"},
                                             {"provider", "legacy"}}})},
                {"default_model", "legacy-model"}};
}

int main() {
    const auto db = std::filesystem::temp_directory_path() /
                    ("test-worker-channel-" + std::to_string(::getpid()));
    std::filesystem::create_directories(db);
    WorkerStore store(db / "workers.db");
    const std::string key = store.enroll("rig1");

    WorkerChannel ch(store, std::stoi(kPort), "127.0.0.1");
    std::thread srv([&] { ch.start(); });

    // Wait for the listener.
    bool up = false;
    for (int i = 0; i < 50 && !up; ++i) {
        try {
            Connection probe{kUrl};
            up = true;
        } catch (...) { std::this_thread::sleep_for(std::chrono::milliseconds(100)); }
    }
    assert(up && "channel never started listening");

    // -- First connection: hello, welcome, online -------------------------
    {
        Connection a{kUrl};
        a.sendText(hello(key).dump());
        auto w = a.receiveText();
        assert(w && w->find("\"welcome\"") != std::string::npos);
        for (int i = 0; i < 30 && !ch.is_online("rig1"); ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        assert(ch.is_online("rig1"));
        // a dies abruptly here (destructor -- no clean close handshake).
    }

    // -- Immediate same-name reconnect (races the old conn's cleanup) -----
    std::atomic<bool> got_ping{false};
    std::atomic<bool> got_welcome{false};
    std::atomic<bool> got_heartbeat_ack{false};
    std::atomic<bool> got_telemetry{false};
    std::mutex stream_m;
    std::condition_variable stream_cv;
    std::string stream_bytes;
    json telemetry_host;
    json telemetry_now;
    ch.on_telemetry = [&](const std::string& worker, const json& host,
                          const json& now) {
        assert(worker == "rig1");
        telemetry_host = host;
        telemetry_now = now;
        got_telemetry = true;
    };
    {
        Connection b{kUrl};
        b.onText([&](std::string msg) {
            if (msg.find("\"welcome\"") != std::string::npos) got_welcome = true;
            if (msg.find("\"ping\"") != std::string::npos) got_ping = true;
            if (msg.find("\"heartbeat_ack\"") != std::string::npos)
                got_heartbeat_ack = true;
            const auto frame = json::parse(msg);
            if (frame.value("op", "") == "stream_open") {
                const std::string sid = frame.value("sid", "");
                const std::string response =
                    "HTTP/1.1 101 Switching Protocols\r\n\r\n";
                // Force the earliest possible arrival: the real worker can
                // relay the bridge's 101 immediately after stream_opened.
                // The hub must already have its receive callback installed;
                // otherwise this first response disappears and /screen/ws
                // waits forever with a blank viewer.
                b.sendText(json{{"op", "stream_data"}, {"sid", sid},
                    {"b64", OmniNet::Crypto::base64Encode(std::span<const uint8_t>(
                        reinterpret_cast<const uint8_t*>(response.data()),
                        response.size()))}}.dump());
                b.sendText(json{{"op", "stream_opened"}, {"sid", sid}}.dump());
            }
        });
        std::thread rl([&] { b.readLoop(); });
        b.sendText(hello(key).dump());

        for (int i = 0; i < 30 && !got_welcome; ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        assert(got_welcome && "reconnect: no welcome");

        // Give the old handler's cleanup a window to run; it must NOT wipe
        // the replacement conn.
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        assert(ch.is_online("rig1") && "replacement conn orphaned by cleanup");

        // Every heartbeat must receive an application-level reply. The
        // worker uses this to detect a black-holed connection whose local
        // sends still succeed (2026-10-28 ubuntu-pc outage).
        hivemind::HostStatsSampler sampler;
        b.sendText(json{{"op", "heartbeat"},
                        {"agents", json::object()},
                        {"host", hivemind::host_facts_json()},
                        {"now", hivemind::host_now_json(sampler.sample())}}.dump());
        for (int i = 0; i < 30 && !got_heartbeat_ack; ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        assert(got_heartbeat_ack && "heartbeat was not acknowledged");
        for (int i = 0; i < 30 && !got_telemetry; ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        assert(got_telemetry && "heartbeat telemetry was not delivered");
        assert(telemetry_host["hostname"].is_string());
        assert(telemetry_now["mem_total_mb"].get<int64_t>() > 0);

        // Channel must accept + deliver work via the writer thread.
        assert(ch.send("rig1", json{{"op", "ping"}, {"n", 1}}));
        for (int i = 0; i < 30 && !got_ping; ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        assert(got_ping && "send() frame never delivered to replacement conn");

        // 2026-11-08 blank-screen regression: bytes arriving with the open
        // acknowledgement must not race callback registration and vanish.
        const std::string sid = ch.open_stream("rig1", 16121,
            [&](std::string_view bytes) {
                {
                    std::lock_guard lk(stream_m);
                    stream_bytes.append(bytes);
                }
                stream_cv.notify_all();
            }, [] {});
        assert(!sid.empty() && "stream did not open");
        {
            std::unique_lock lk(stream_m);
            const bool arrived = stream_cv.wait_for(lk, std::chrono::seconds(2),
                [&] { return !stream_bytes.empty(); });
            assert(arrived && "first bridge bytes were dropped during stream open");
            assert(stream_bytes == "HTTP/1.1 101 Switching Protocols\r\n\r\n");
        }
        ch.stream_close(sid);

        b.close();
        // NOTE: OmniNet client close() does not wake a readLoop blocked in
        // recv -- detach instead of joining (thread dies at process exit).
        rl.detach();
    }

    ch.stop();
    std::cerr << "[t] ch stopped, joining srv\n";
    srv.join();
    std::cerr << "[t] srv joined\n";
    std::filesystem::remove_all(db);
    std::cout << "test_worker_channel: OK\n";
    return 0;
}
