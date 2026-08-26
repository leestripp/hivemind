// hive-worker — Phase 30: remote container executor skeleton (30.2).
//
// Dials OUT to the hive-server worker channel and holds the connection:
//   hello (key auth) → welcome → heartbeat every 15s, receive loop for
//   server frames (exec arrives in 30.3), reconnect with backoff.
//
// Usage: hive-worker --server ws://host:8423 --key hmwk_... --name rig-gpu
//                    [--caps '{"gpu":true}']

#include "websocket/WebSocket.h"

#include <nlohmann/json.hpp>

#include <atomic>
#include <csignal>
#include <chrono>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>

using json = nlohmann::json;
using OmniNet::WebSocket::Connection;

// No custom SIGINT/SIGTERM handler on purpose: the default disposition
// kills the process, the OS closes the socket, and the server's receive
// loop immediately observes the disconnect (correct `online` flag).
// A flag-based graceful stop would hang in the blocking receiveText().
static std::atomic<bool> g_stop{false};

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

            // heartbeat sender; receive loop below drives reconnect
            std::atomic<bool> dead{false};
            std::thread hb([&] {
                while (!g_stop && !dead) {
                    std::this_thread::sleep_for(std::chrono::seconds(15));
                    if (g_stop || dead) break;
                    try {
                        auto up = std::chrono::duration_cast<std::chrono::seconds>(
                            std::chrono::steady_clock::now() - connected_at).count();
                        ws.sendText(json{{"op","heartbeat"},{"uptime_s",up}}.dump());
                    } catch (...) { dead = true; }
                }
            });

            while (!g_stop && !dead) {
                auto msg = ws.receiveText();
                if (!msg) break;   // closed
                json frame;
                try { frame = json::parse(*msg); } catch (...) { continue; }
                std::cout << "worker: frame: " << frame.dump() << "\n";
                // 30.3: dispatch {"op":"exec",...} to ContainerManager here.
            }
            dead = true;
            if (hb.joinable()) hb.join();
        } catch (const std::exception& e) {
            std::cerr << "worker: connect failed: " << e.what() << "\n";
        }
        if (g_stop) break;
        std::cerr << "worker: reconnecting in " << backoff << "s\n";
        std::this_thread::sleep_for(std::chrono::seconds(backoff));
        backoff = std::min(backoff * 2, 30);
    }
    std::cout << "worker: stopped\n";
    return 0;
}
