// ServiceTools.cpp — see ServiceTools.h for the contract.

#include "tools/ServiceTools.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

namespace fs = std::filesystem;

namespace hive {
namespace {

// ── environment (injected by hive-worker at container create) ────────

struct ServiceEnv {
    std::string host;
    int port_base = 0;
    int port_count = 0;
    int container_base = 8000;
    bool ok = false;   // host/port_base/port_count all present & sane
};

int env_int(const char* key, int fallback) {
    const char* v = std::getenv(key);
    if (!v || !*v) return fallback;
    const int n = std::atoi(v);
    return n > 0 ? n : fallback;
}

ServiceEnv service_env() {
    ServiceEnv e;
    const char* host = std::getenv("HIVEMIND_SERVICE_HOST");
    if (host && *host) e.host = host;
    e.port_base = env_int("HIVEMIND_SERVICE_PORT_BASE", 0);
    e.port_count = env_int("HIVEMIND_SERVICE_PORT_COUNT", 0);
    e.container_base = env_int("HIVEMIND_SERVICE_CONTAINER_BASE", 8000);
    e.ok = !e.host.empty() && e.port_base > 0 && e.port_count > 0;
    return e;
}

json env_error() {
    return {{"error",
             "service sharing is not configured for this container: "
             "HIVEMIND_SERVICE_HOST / _PORT_BASE / _PORT_COUNT are unset. "
             "The worker injects them at container create — this container "
             "predates the feature; it picks them up on next recreate."}};
}

// ── manifest ($HIVE_WORKSPACE/.hivemind/services.json) ───────────────

fs::path manifest_path() {
    const char* ws = std::getenv("HIVE_WORKSPACE");
    return fs::path(ws && *ws ? ws : "/workspace") / ".hivemind" /
           "services.json";
}

json read_manifest() {
    std::ifstream in(manifest_path());
    if (!in) return {{"version", 1}, {"services", json::array()}};
    try {
        json m = json::parse(in);
        if (!m.contains("services") || !m["services"].is_array())
            m["services"] = json::array();
        return m;
    } catch (...) {
        // Corrupt manifest: back it aside rather than destroy evidence.
        std::error_code ec;
        fs::rename(manifest_path(), manifest_path().string() + ".corrupt", ec);
        return {{"version", 1}, {"services", json::array()}};
    }
}

void write_manifest(const json& m) {
    const fs::path p = manifest_path();
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    const fs::path tmp = p.string() + ".tmp";
    {
        std::ofstream out(tmp, std::ios::trunc);
        out << m.dump(2) << '\n';
    }
    fs::rename(tmp, p, ec);
    if (ec) {
        fs::copy_file(tmp, p, fs::copy_options::overwrite_existing, ec);
        fs::remove(tmp, ec);
    }
}

// ── health check: TCP connect to 127.0.0.1:port (+ optional GET) ─────

struct Health {
    bool connected = false;
    int http_status = 0;   // 0 = no HTTP probe or no response parsed
};

Health check_health(int port, const std::string& health_path) {
    Health h;
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return h;
    // 2s send/recv timeout keeps the tool snappy on a hung listener.
    const timeval tv{2, 0};
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(port));
    ::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0) {
        ::close(fd);
        return h;
    }
    h.connected = true;
    if (!health_path.empty()) {
        const std::string req = "GET " + health_path +
                                " HTTP/1.1\r\nHost: 127.0.0.1\r\n"
                                "Connection: close\r\n\r\n";
        if (::send(fd, req.data(), req.size(), 0) > 0) {
            char buf[256];
            const ssize_t n = ::recv(fd, buf, sizeof buf - 1, 0);
            if (n > 0) {
                buf[n] = '\0';
                int status = 0;
                if (std::sscanf(buf, "HTTP/%*d.%*d %d", &status) == 1)
                    h.http_status = status;
            }
        }
    }
    ::close(fd);
    return h;
}

// ── shared helpers ────────────────────────────────────────────────────

bool valid_name(const std::string& s) {
    if (s.empty() || s.size() > 64) return false;
    return std::all_of(s.begin(), s.end(), [](unsigned char c) {
        return std::isalnum(c) || c == '-' || c == '_';
    });
}

std::string service_url(const ServiceEnv& e, int port) {
    return "http://" + e.host + ":" +
           std::to_string(e.port_base + (port - e.container_base));
}

json entry_with_health(json entry, const ServiceEnv& e) {
    const int port = entry.value("port", 0);
    const std::string hp = entry.value("health_path", std::string{});
    const Health h = check_health(port, hp);
    entry["healthy"] = h.connected &&
                       (hp.empty() || (h.http_status >= 200 && h.http_status < 400));
    entry["http_status"] = h.http_status;
    entry["url"] = service_url(e, port);
    entry["host_port"] = e.port_base + (port - e.container_base);
    return entry;
}

} // namespace

// ── share_service ─────────────────────────────────────────────────────

json ShareServiceTool::execute(const json& args, HostToolContext& ctx) {
    (void)ctx;
    const ServiceEnv e = service_env();
    if (!e.ok) return env_error();

    const std::string name = args.value("name", std::string{});
    const int port = args.value("port", 0);
    const std::string health_path = args.value("health_path", std::string{});

    if (!valid_name(name))
        return {{"error",
                 "invalid name: use 1-64 chars of [a-zA-Z0-9_-]"}};
    if (port < e.container_base || port >= e.container_base + e.port_count)
        return {{"error",
                 "port " + std::to_string(port) + " is outside your shared "
                 "range " + std::to_string(e.container_base) + "-" +
                 std::to_string(e.container_base + e.port_count - 1) +
                 " — bind your service to a port in that range and retry"}};

    json m = read_manifest();
    bool changed = true;
    json* slot = nullptr;
    for (auto& s : m["services"]) {
        if (s.value("name", std::string{}) == name) { slot = &s; break; }
    }
    json entry = slot ? *slot : json::object();
    if (slot && slot->value("port", 0) == port &&
        slot->value("health_path", std::string{}) == health_path)
        changed = false;
    entry["name"] = name;
    entry["port"] = port;
    entry["health_path"] = health_path;
    if (changed || !entry.contains("shared_at")) {
        const auto now = std::chrono::system_clock::to_time_t(
            std::chrono::system_clock::now());
        char ts[32];
        std::strftime(ts, sizeof ts, "%Y-%m-%dT%H:%M:%SZ",
                      std::gmtime(&now));
        entry["shared_at"] = ts;
    }
    entry = entry_with_health(entry, e);
    if (slot) *slot = entry;
    else m["services"].push_back(entry);
    write_manifest(m);

    json out = entry;
    out["changed"] = changed;
    return out;
}

// ── list_services ─────────────────────────────────────────────────────

json ListServicesTool::execute(const json& args, HostToolContext& ctx) {
    (void)args;
    (void)ctx;
    const ServiceEnv e = service_env();
    if (!e.ok) return env_error();

    json services = json::array();
    for (auto& s : read_manifest()["services"])
        services.push_back(entry_with_health(s, e));
    return {{"services", services},
            {"host", e.host},
            {"container_base", e.container_base},
            {"port_count", e.port_count},
            {"port_base", e.port_base},
            {"hint",
             "bind a service to any port in " +
                 std::to_string(e.container_base) + "-" +
                 std::to_string(e.container_base + e.port_count - 1) +
                 " and share_service(name, port) to publish it"}};
}

// ── unshare_service ───────────────────────────────────────────────────

json UnshareServiceTool::execute(const json& args, HostToolContext& ctx) {
    (void)ctx;
    const ServiceEnv e = service_env();
    if (!e.ok) return env_error();

    const std::string name = args.value("name", std::string{});
    if (!valid_name(name))
        return {{"error", "invalid name"}};

    json m = read_manifest();
    auto& arr = m["services"];
    const auto it = std::find_if(arr.begin(), arr.end(), [&](const json& s) {
        return s.value("name", std::string{}) == name;
    });
    if (it == arr.end())
        return {{"error", "no shared service named \"" + name + "\""}};
    const json removed = *it;
    arr.erase(it);
    write_manifest(m);
    return {{"removed", true},
            {"name", name},
            {"url", removed.value("url", std::string{})}};
}

} // namespace hive
