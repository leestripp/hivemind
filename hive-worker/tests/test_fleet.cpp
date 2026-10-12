// test_fleet - Phase B standalone harness: drives hwfleet::Fleet directly
// (no server, no channel). Spawns one agent container, sends a task over
// the NDJSON session, expects a message event containing the exact reply.
//
// Env required: HIVE_TEST_API_KEY (kimi key), optional HIVE_TEST_MODEL.
// Run from the REPO ROOT (image build context convention).

#include "../src/fleet.h"
#include "../src/runner.h"

#include <sys/stat.h>

#include <cassert>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <vector>

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

    // Phase 65: whole-spec equality drives Fleet::spawn's respawn dedup —
    // identical re-spawn no-ops, any env-affecting change restarts.
    {
        hwfleet::AgentSpec a;
        a.id = "agent-1"; a.name = "Bee"; a.model = "kimi-for-coding";
        a.secret_env = "HIVE_SECRET_ONE=abc\n";
        hwfleet::AgentSpec b = a;
        assert(a == b);                          // copy is identical
        b.secret_env += "HIVE_SECRET_TWO=def\n";
        assert(a != b);                          // new grant -> restart
        b = a;
        b.mcp_servers = json::array({{"command","x"}});
        assert(a != b);                          // mcp change -> restart
        b = a;
        b.disabled_skills.push_back("web");
        assert(a != b);                          // skill gate -> restart
        b = a;
        b.temperature = 0.5;
        assert(a != b);                          // tuning change -> restart
        b = a;
        b.memory = "32g";
        assert(a != b);                          // Phase 69: limits change -> restart
        b = a;
        b.cpus = 12;
        assert(a != b);                          // Phase 69
        std::cout << "test_fleet: AgentSpec equality OK\n";
    }

    // Phase 69: podman size parsing + cpu formatting (pure).
    {
        using hwrunner::parse_memory_bytes;
        using hwrunner::format_cpus;
        assert(parse_memory_bytes("4g")    == 4LL * 1024 * 1024 * 1024);
        assert(parse_memory_bytes("32g")   == 32LL * 1024 * 1024 * 1024);
        assert(parse_memory_bytes("1536m") == 1536LL * 1024 * 1024);
        assert(parse_memory_bytes("512")   == 512);
        assert(parse_memory_bytes("1t")    == 1024LL * 1024 * 1024 * 1024);
        assert(parse_memory_bytes("2G")    == 2LL * 1024 * 1024 * 1024);
        assert(parse_memory_bytes("")      == -1);
        assert(parse_memory_bytes("abc")   == -1);
        assert(parse_memory_bytes("4gb")   == -1);   // two-char suffix
        assert(parse_memory_bytes("g")     == -1);   // no digits
        assert(parse_memory_bytes("-4g")   == -1);   // negative
        assert(format_cpus(12)   == "12");
        assert(format_cpus(1.5)  == "1.5");
        assert(format_cpus(2)    == "2");
        assert(format_cpus(0)    == "0");
        std::cout << "test_fleet: parse_memory_bytes/format_cpus OK\n";
    }

    // Phase 72: host-agent unix user mapping (pure).
    {
        using hwrunner::host_user;
        assert(host_user("Rooty")      == "hivex-rooty");   // lowercased
        assert(host_user("SysAdmin-2") == "hivex-sysadmin-2");
        assert(host_user("Odd Name!")  == "hivex-odd-name-");  // clamped
        // Linux username cap: 32 chars total.
        assert(host_user(std::string(40, 'a')) ==
               "hivex-" + std::string(26, 'a'));
        std::cout << "test_fleet: host_user OK\n";
    }

    // Phase 72: host_workspace — per-agent dir under the configured root,
    // home dir when unset. NEVER cleaned up (workspace is permanent).
    {
        using hwrunner::host_workspace;
        hwrunner::g_host_workspace_root = "";
        assert(host_workspace("TestBot") == "/home/hivex-testbot");
        // Shared: the root IS the workspace, for every agent alike.
        hwrunner::g_host_workspace_root = "/workspace";
        assert(host_workspace("TestBot") == "/workspace");
        assert(host_workspace("OtherBot") == "/workspace");
        hwrunner::g_host_workspace_root = "";   // leave clean
        std::cout << "test_fleet: host_workspace OK\n";
    }

    // 2026-10-12: runtime descriptor env (pure). Container agents keep
    // the mount paths + no service env (that rides podman -e); host
    // agents get their real workspace root, the host skill library path,
    // and an IDENTITY service-port mapping (container_base == port_base).
    {
        hwfleet::RuntimeEnv c;
        c.runtime = "container"; c.workspace_root = "/workspace";
        c.skills_dir = "/skills";
        const std::string cl = hwfleet::runtime_env_lines(c);
        assert(cl.find("HIVEMIND_RUNTIME=container\n") != std::string::npos);
        assert(cl.find("HIVEMIND_WORKSPACE_ROOT=/workspace\n") != std::string::npos);
        assert(cl.find("HIVEMIND_SKILLS_DIR=/skills\n") != std::string::npos);
        assert(cl.find("HIVEMIND_SERVICE") == std::string::npos);   // unset

        hwfleet::RuntimeEnv h;
        h.runtime = "host"; h.workspace_root = "/workspace";
        h.skills_dir = "/home/lee/.local/share/hive-worker/skills";
        h.service_host = "192.168.1.10";
        h.service_port_base = 18400; h.service_port_count = 10;
        const std::string hl = hwfleet::runtime_env_lines(h);
        assert(hl.find("HIVEMIND_RUNTIME=host\n") != std::string::npos);
        assert(hl.find("HIVEMIND_SKILLS_DIR=/home/lee/.local/share/"
                       "hive-worker/skills\n") != std::string::npos);
        assert(hl.find("HIVEMIND_SERVICE_HOST=192.168.1.10\n") != std::string::npos);
        assert(hl.find("HIVEMIND_SERVICE_PORT_BASE=18400\n") != std::string::npos);
        assert(hl.find("HIVEMIND_SERVICE_PORT_COUNT=10\n") != std::string::npos);
        // identity: agent binds the advertised port directly
        assert(hl.find("HIVEMIND_SERVICE_CONTAINER_BASE=18400\n") != std::string::npos);

        // Newlines are flattened (env files are line-based).
        hwfleet::RuntimeEnv n;
        n.runtime = "host"; n.workspace_root = "/ws\nevil=1";
        n.skills_dir = "/skills";
        const std::string nl = hwfleet::runtime_env_lines(n);
        // ...so it can never become a KEY=VALUE line of its own.
        assert(nl.find("\nevil=") == std::string::npos);
        assert(nl.find("/ws evil=1\n") != std::string::npos);
        std::cout << "test_fleet: runtime_env_lines OK\n";
    }

    // Phase 72: spawn-path gating — a host agent must be REFUSED when the
    // worker hasn't opted in, and when opted in but the hivex user isn't
    // provisioned. Both throw from start() before any fork happens (pure:
    // no podman, no sudo, no user creation in the test).
    {
        hwfleet::AgentSpec hs;
        hs.id = "hostbee"; hs.name = "HostBee-NoSuchUser-Xyz";
        hs.host_agent = true;
        hwfleet::AgentProcess proc(hs, [](const std::string&, const json&) {});

        hwrunner::g_allow_host_agents = false;
        bool threw = false;
        try { proc.start(); } catch (const std::runtime_error&) { threw = true; }
        assert(threw);   // refused: worker not opted in

        hwrunner::g_allow_host_agents = true;
        threw = false;
        try { proc.start(); } catch (const std::runtime_error&) { threw = true; }
        assert(threw);   // refused: hivex-hostbee-nosuchuser-xyz not provisioned
        hwrunner::g_allow_host_agents = false;   // leave the global clean
        std::cout << "test_fleet: host-agent spawn gating OK\n";
    }

    // Phase 65b: a task queued while the exec is down must not wedge its
    // hub-side turn when the process is stopped — stop() fails pending
    // task frames with a synthetic error "message" event (regression:
    // 2026-09-25 PA wedge, exec died mid-turn and the hub turn stuck).
    {
        std::vector<json> events;
        hwfleet::AgentSpec ws;
        ws.id = "wedgebee"; ws.name = "WedgeBee";
        hwfleet::AgentProcess proc(
            ws, [&](const std::string&, const json& ev) {
                events.push_back(ev);
            });
        // stdin down (never started): both frames land in pending_.
        proc.send({{"op","task"},{"id","tw1"},{"thread_id","thw"},
                   {"text","hi"}});
        proc.send({{"op","cancel"}});   // non-task frame: queued, not failed
        proc.stop();                    // never started: fast path, no child
        assert(events.size() == 1);     // only the task frame fails
        assert(events[0].value("ev","") == "message");
        assert(events[0].value("task","") == "tw1");
        assert(events[0].value("thread_id","") == "thw");
        assert(events[0].value("body","x").empty());
        assert(!events[0].value("error","").empty());
        std::cout << "test_fleet: pending-task fail-on-stop OK\n";
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
