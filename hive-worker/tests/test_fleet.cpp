// test_fleet - Phase B standalone harness: drives hwfleet::Fleet directly
// (no server, no channel). Spawns one agent container, sends a task over
// the NDJSON session, expects a message event containing the exact reply.
//
// Env required: HIVE_TEST_API_KEY (kimi key), optional HIVE_TEST_MODEL.
// Run from the REPO ROOT (image build context convention).

#include "../src/fleet.h"

#include <sys/stat.h>

#include <cassert>
#include <filesystem>
#include <fstream>
#include <stdexcept>

#include <condition_variable>
#include <cstdlib>
#include <iostream>
#include <mutex>

using nlohmann::json;
namespace fs = std::filesystem;

int main() {
    // Phase 35: type -> image mapping (pure; runs without any API key or
    // containers so it always executes, even in the early-return path).
    {
        hwfleet::AgentSpec s;
        assert(hwfleet::resolved_image(s) == hwfleet::kImageLatest);
        s.type = "gpu";
        assert(hwfleet::resolved_image(s) == hwfleet::kImageGpu);
        s.image = "localhost/custom:tag";       // explicit override wins
        assert(hwfleet::resolved_image(s) == "localhost/custom:tag");
        std::cout << "test_fleet: resolved_image mapping OK\n";
    }

    // (Phase 43: resolve_model + model-registry ops moved to the hub's
    // LlmStore — tested by test_llmstore.)

    const char* key = std::getenv("HIVE_TEST_API_KEY");
    // 77 = ctest SKIP_RETURN_CODE: pure tests above already ran; the
    // container half needs a live Kimi key and is skipped under ctest.
    if (!key || !*key) { std::cerr << "HIVE_TEST_API_KEY required (skipping container half)\n"; return 77; }

    // secrets.json for resolve_api_key()
    system("mkdir -p ~/.local/share/hive-worker");
    {
        std::string path = std::string(std::getenv("HOME"))
                         + "/.local/share/hive-worker/secrets.json";
        FILE* f = fopen(path.c_str(), "w");
        fprintf(f, "{\"kimi\":\"%s\"}\n", key);
        fclose(f);
        chmod(path.c_str(), 0600);
    }

    std::mutex m; std::condition_variable cv;
    json message; int deltas = 0; bool ready = false;

    hwfleet::Fleet fleet([&](const std::string& aid, const json& ev) {
        std::lock_guard<std::mutex> lk(m);
        const std::string k = ev.value("ev","");
        std::cerr << "[ev " << aid << "] " << k << "\n";
        if (k == "ready") ready = true;
        else if (k == "delta") ++deltas;
        else if (k == "message") { message = ev; cv.notify_all(); }
        else if (k == "status" || k == "error")
            std::cerr << "  " << ev.dump() << "\n";
    });

    hwfleet::AgentSpec spec;
    spec.id            = "testbee1";
    spec.name          = "TestBee";
    spec.model         = "kimi-for-coding";
    spec.api_host      = "https://api.kimi.com";
    spec.chat_endpoint = "/coding/v1/chat/completions";
    spec.provider      = "kimi";
    spec.max_context   = 256000;
    spec.temperature   = 1.0;
    spec.auto_approve  = true;   // headless test: no approval round-trips

    try { fleet.spawn(spec); }
    catch (const std::exception& e) {
        std::cerr << "spawn failed: " << e.what() << "\n"; return 1;
    }

    // wait for ready
    {
        std::unique_lock<std::mutex> lk(m);
        if (!cv.wait_for(lk, std::chrono::seconds(90), [&]{ return ready; })) {
            std::cerr << "FAIL: no ready event\n"; return 1;
        }
    }
    fleet.send_to("testbee1", {{"op","task"},{"id","t1"},
        {"text","Reply with exactly: FLEET ALIVE"},{"thread_id","th1"}});

    {
        std::unique_lock<std::mutex> lk(m);
        if (!cv.wait_for(lk, std::chrono::seconds(180),
                         [&]{ return !message.is_null(); })) {
            std::cerr << "FAIL: no message event\n"; return 1;
        }
    }
    const std::string body = message.value("body","");
    std::cerr << "message: " << body.substr(0,120) << "\n";
    std::cerr << "deltas: " << deltas << "\n";
    fleet.stop("testbee1");

    if (body.find("FLEET ALIVE") == std::string::npos) {
        std::cerr << "FAIL: body mismatch\n"; return 1;
    }
    std::cout << "PASS\n";
    return 0;
}
