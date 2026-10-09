// test_service_tools.cpp — share_service / list_services /
// unshare_service (2026-10-09, Bob/SlateApe ingress).
//
// Spins a real loopback HTTP listener so health checks are exercised
// end-to-end; env vars stand in for the worker-injected container env.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include "tools/ServiceTools.h"

using nlohmann::json;
namespace fs = std::filesystem;

namespace {

// Minimal loopback HTTP server: answers every request with 200 OK.
struct StubServer {
    int fd = -1;
    std::atomic<bool> stop{false};
    std::thread th;

    bool start(int port) {
        fd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) return false;
        int one = 1;
        ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(static_cast<uint16_t>(port));
        ::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
        if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0 ||
            ::listen(fd, 4) != 0) {
            ::close(fd);
            fd = -1;
            return false;
        }
        th = std::thread([this] {
            while (!stop) {
                const int c = ::accept(fd, nullptr, nullptr);
                if (c < 0) break;
                char buf[512];
                ::recv(c, buf, sizeof buf, 0);
                static const char resp[] =
                    "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok";
                ::send(c, resp, sizeof resp - 1, 0);
                ::close(c);
            }
        });
        return true;
    }

    ~StubServer() {
        stop = true;
        if (fd >= 0) ::shutdown(fd, SHUT_RDWR);
        if (th.joinable()) th.join();
        if (fd >= 0) ::close(fd);
    }
};

// HostToolContext stub — the service tools never touch it.
struct StubCtx : hive::HostToolContext {
    std::filesystem::path root;
    std::string read_file(const std::filesystem::path&) override { return {}; }
    void write_file(const std::filesystem::path&,
                    const std::string&) override {}
    std::vector<std::string>
        list_directory(const std::filesystem::path&) override { return {}; }
    const std::filesystem::path& project_root() const override { return root; }
};

struct EnvFixture {
    fs::path ws;
    EnvFixture() {
        ws = fs::temp_directory_path() / "hive-test-svctools";
        fs::remove_all(ws);
        fs::create_directories(ws);
        setenv("HIVE_WORKSPACE", ws.c_str(), 1);
        setenv("HIVEMIND_SERVICE_HOST", "127.0.0.1", 1);
        setenv("HIVEMIND_SERVICE_PORT_BASE", "28400", 1);
        setenv("HIVEMIND_SERVICE_PORT_COUNT", "10", 1);
        unsetenv("HIVEMIND_SERVICE_CONTAINER_BASE");
    }
    ~EnvFixture() { fs::remove_all(ws); }
};

constexpr int kPort = 8003;   // container_base 8000 + offset 3 → host 28403

} // namespace

TEST_CASE("share/list/unshare round-trip with live health check") {
    EnvFixture env;
    StubCtx ctx;
    StubServer srv;
    REQUIRE(srv.start(kPort));

    hive::ShareServiceTool share;
    hive::ListServicesTool list;
    hive::UnshareServiceTool unshare;

    SUBCASE("share returns URL + health") {
        const json r = share.execute(
            {{"name", "demo"}, {"port", kPort}, {"health_path", "/healthz"}},
            ctx);
        CHECK_FALSE(r.contains("error"));
        CHECK(r["url"] == "http://127.0.0.1:28403");
        CHECK(r["host_port"] == 28403);
        CHECK(r["healthy"] == true);
        CHECK(r["http_status"] == 200);
        CHECK(r["changed"] == true);
        CHECK(fs::exists(env.ws / ".hivemind" / "services.json"));
    }

    SUBCASE("share is idempotent") {
        share.execute({{"name", "demo"}, {"port", kPort}}, ctx);
        const json r = share.execute({{"name", "demo"}, {"port", kPort}}, ctx);
        CHECK(r["changed"] == false);
    }

    SUBCASE("out-of-range port is rejected") {
        const json r = share.execute({{"name", "bad"}, {"port", 9000}}, ctx);
        REQUIRE(r.contains("error"));
        CHECK(r["error"].get<std::string>().find("outside your shared range")
              != std::string::npos);
    }

    SUBCASE("invalid name is rejected") {
        const json r = share.execute(
            {{"name", "bad name!"}, {"port", kPort}}, ctx);
        CHECK(r.contains("error"));
    }

    SUBCASE("list shows the entry with live health") {
        share.execute({{"name", "demo"}, {"port", kPort}}, ctx);
        const json r = list.execute(json::object(), ctx);
        REQUIRE(r["services"].size() == 1);
        CHECK(r["services"][0]["name"] == "demo");
        CHECK(r["services"][0]["healthy"] == true);
        CHECK(r["port_base"] == 28400);
        CHECK(r["container_base"] == 8000);
    }

    SUBCASE("unshare removes the entry; repeat errors") {
        share.execute({{"name", "demo"}, {"port", kPort}}, ctx);
        const json r = unshare.execute({{"name", "demo"}}, ctx);
        CHECK(r["removed"] == true);
        CHECK(list.execute(json::object(), ctx)["services"].empty());
        CHECK(unshare.execute({{"name", "demo"}}, ctx).contains("error"));
    }
}

TEST_CASE("missing worker env yields a clear error, not a crash") {
    EnvFixture env;
    StubCtx ctx;
    unsetenv("HIVEMIND_SERVICE_PORT_BASE");
    hive::ShareServiceTool share;
    const json r = share.execute({{"name", "demo"}, {"port", kPort}}, ctx);
    REQUIRE(r.contains("error"));
    CHECK(r["error"].get<std::string>().find("HIVEMIND_SERVICE_")
          != std::string::npos);
}
