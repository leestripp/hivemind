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
#include "hivemind/WorkerStore.h"
#include "websocket/WebSocket.h"

#include <nlohmann/json.hpp>

#include <atomic>
#include <cassert>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <thread>

using nlohmann::json;
using OmniNet::WebSocket::Connection;

static const char* kPort = "18491";
static const std::string kUrl =
    std::string("ws://127.0.0.1:") + kPort + "/api/workers/channel";

static json hello(const std::string& key) {
    return json{{"op", "hello"},   {"worker", "rig1"}, {"key", key},
                {"caps", json::object()}, {"proto", 1}, {"models", json::array()}};
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
    {
        Connection b{kUrl};
        b.onText([&](std::string msg) {
            if (msg.find("\"welcome\"") != std::string::npos) got_welcome = true;
            if (msg.find("\"ping\"") != std::string::npos) got_ping = true;
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

        // Channel must accept + deliver work via the writer thread.
        assert(ch.send("rig1", json{{"op", "ping"}, {"n", 1}}));
        for (int i = 0; i < 30 && !got_ping; ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        assert(got_ping && "send() frame never delivered to replacement conn");

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
