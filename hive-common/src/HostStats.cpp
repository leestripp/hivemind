#include "hivemind/HostStats.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sys/statvfs.h>
#include <sys/utsname.h>
#include <unistd.h>

namespace hivemind {
namespace {

bool read_double(const nlohmann::json& j, const char* key, double& out) {
    if (!j.contains(key) || !j[key].is_number()) return false;
    out = j[key].get<double>();
    return std::isfinite(out);
}

bool read_i64(const nlohmann::json& j, const char* key, int64_t& out) {
    if (!j.contains(key) || !j[key].is_number_integer()) return false;
    out = j[key].get<int64_t>();
    return true;
}

bool valid_pct(double value) { return value >= 0 && value <= 100; }

}  // namespace

HostStatsSample HostStatsSampler::sample() {
    HostStatsSample s;
    s.t = static_cast<int64_t>(time(nullptr));

    if (std::ifstream in{"/proc/stat"}; in) {
        std::string cpu;
        uint64_t user = 0, nice = 0, sys = 0, idle = 0, iowait = 0;
        uint64_t irq = 0, soft = 0;
        in >> cpu >> user >> nice >> sys >> idle >> iowait >> irq >> soft;
        const uint64_t total = user + nice + sys + idle + iowait + irq + soft;
        const uint64_t idl = idle + iowait;
        if (prev_total_ && total > prev_total_ && idl >= prev_idle_) {
            const double dt = static_cast<double>(total - prev_total_);
            const double di = static_cast<double>(idl - prev_idle_);
            s.cpu_pct = std::clamp(100.0 * (1.0 - di / dt), 0.0, 100.0);
        }
        prev_total_ = total;
        prev_idle_ = idl;
    }

    if (std::ifstream in{"/proc/meminfo"}; in) {
        std::string key, unit;
        int64_t val = 0;
        while (in >> key >> val >> unit) {
            if (key == "MemTotal:") s.mem_total_kb = val;
            if (key == "MemAvailable:") s.mem_avail_kb = val;
            if (s.mem_total_kb && s.mem_avail_kb) break;
        }
    }

    if (std::ifstream in{"/proc/loadavg"}; in) in >> s.load1;

    struct statvfs vfs {};
    if (statvfs("/", &vfs) == 0 && vfs.f_blocks) {
        s.disk_total_kb = static_cast<int64_t>(vfs.f_blocks) *
                          vfs.f_frsize / 1024;
        s.disk_avail_kb = static_cast<int64_t>(vfs.f_bavail) *
                          vfs.f_frsize / 1024;
        s.disk_pct = 100.0 * (1.0 - static_cast<double>(s.disk_avail_kb) /
                                      static_cast<double>(s.disk_total_kb));
    }

    if (std::ifstream in{"/proc/self/status"}; in) {
        std::string line;
        while (std::getline(in, line)) {
            if (line.rfind("VmRSS:", 0) == 0) {
                s.process_rss_kb = std::strtoll(line.c_str() + 6, nullptr, 10);
                break;
            }
        }
    }

    s.containers = count_containers();
    return s;
}

int HostStatsSampler::count_containers() {
    if (!podman_checked_) {
        podman_ok_ = std::system("command -v podman >/dev/null 2>&1") == 0;
        podman_checked_ = true;
    }
    if (!podman_ok_) return 0;
    std::array<char, 64> buf{};
    std::string out;
    if (FILE* p = popen(
            "podman ps --filter name=^hive- -q 2>/dev/null | wc -l", "r")) {
        while (fgets(buf.data(), static_cast<int>(buf.size()), p))
            out += buf.data();
        pclose(p);
    }
    return out.empty() ? 0 : std::atoi(out.c_str());
}

HostFacts host_facts() {
    HostFacts facts;
    char host[256] = {0};
    gethostname(host, sizeof(host) - 1);
    facts.hostname = host;
    struct utsname uts {};
    if (uname(&uts) == 0) facts.kernel = uts.release;
    facts.cores = static_cast<int>(std::max<long>(0, sysconf(_SC_NPROCESSORS_ONLN)));
    double uptime = 0;
    if (std::ifstream in{"/proc/uptime"}; in) in >> uptime;
    facts.uptime_s = static_cast<int64_t>(uptime);
    return facts;
}

nlohmann::json host_facts_json(const HostFacts& facts) {
    return {{ "hostname", facts.hostname }, { "kernel", facts.kernel },
            { "cores", facts.cores }, { "uptime_s", facts.uptime_s }};
}

nlohmann::json host_facts_json() { return host_facts_json(host_facts()); }

nlohmann::json host_now_json(const HostStatsSample& s) {
    const double mem_pct = s.mem_total_kb
        ? 100.0 * (1.0 - static_cast<double>(s.mem_avail_kb) /
                           static_cast<double>(s.mem_total_kb))
        : 0;
    return {
        {"t", s.t}, {"cpu_pct", s.cpu_pct}, {"load1", s.load1},
        {"mem_total_mb", s.mem_total_kb / 1024},
        {"mem_used_mb", (s.mem_total_kb - s.mem_avail_kb) / 1024},
        {"mem_pct", mem_pct},
        {"disk_total_gb", s.disk_total_kb / 1048576},
        {"disk_used_gb", (s.disk_total_kb - s.disk_avail_kb) / 1048576},
        {"disk_pct", s.disk_pct},
        {"rss_mb", s.process_rss_kb / 1024},
        {"containers", s.containers},
    };
}

nlohmann::json host_history_json(const HostStatsSample& s) {
    const double mem_pct = s.mem_total_kb
        ? 100.0 * (1.0 - static_cast<double>(s.mem_avail_kb) /
                           static_cast<double>(s.mem_total_kb))
        : 0;
    return {{"t", s.t}, {"cpu", s.cpu_pct}, {"mem", mem_pct},
            {"load1", s.load1}, {"disk", s.disk_pct},
            {"rss_mb", s.process_rss_kb / 1024},
            {"containers", s.containers}};
}

std::optional<HostFacts> host_facts_from_json(const nlohmann::json& value) {
    if (!value.is_object() || !value.contains("hostname") ||
        !value["hostname"].is_string() || !value.contains("kernel") ||
        !value["kernel"].is_string())
        return std::nullopt;
    HostFacts facts;
    facts.hostname = value["hostname"].get<std::string>();
    facts.kernel = value["kernel"].get<std::string>();
    int64_t cores = 0;
    if (facts.hostname.empty() || facts.hostname.size() > 255 ||
        facts.kernel.size() > 255 ||
        !read_i64(value, "cores", cores) || cores < 0 || cores > 65536 ||
        !read_i64(value, "uptime_s", facts.uptime_s) || facts.uptime_s < 0)
        return std::nullopt;
    facts.cores = static_cast<int>(cores);
    return facts;
}

std::optional<HostStatsSample> host_sample_from_json(
    const nlohmann::json& j) {
    if (!j.is_object()) return std::nullopt;
    HostStatsSample s;
    int64_t mem_used_mb = 0, disk_used_gb = 0, rss_mb = 0, containers = 0;
    int64_t mem_total_mb = 0, disk_total_gb = 0;
    double mem_pct = 0;
    if (!read_i64(j, "t", s.t) || s.t <= 0 ||
        !read_double(j, "cpu_pct", s.cpu_pct) || !valid_pct(s.cpu_pct) ||
        !read_double(j, "load1", s.load1) || s.load1 < 0 ||
        !read_i64(j, "mem_total_mb", mem_total_mb) || mem_total_mb <= 0 ||
        !read_i64(j, "mem_used_mb", mem_used_mb) || mem_used_mb < 0 ||
        mem_used_mb > mem_total_mb ||
        !read_double(j, "mem_pct", mem_pct) || !valid_pct(mem_pct) ||
        !read_i64(j, "disk_total_gb", disk_total_gb) || disk_total_gb <= 0 ||
        !read_i64(j, "disk_used_gb", disk_used_gb) || disk_used_gb < 0 ||
        disk_used_gb > disk_total_gb ||
        !read_double(j, "disk_pct", s.disk_pct) || !valid_pct(s.disk_pct) ||
        !read_i64(j, "rss_mb", rss_mb) || rss_mb < 0 ||
        !read_i64(j, "containers", containers) || containers < 0 ||
        containers > 100000)
        return std::nullopt;

    s.mem_total_kb = mem_total_mb * 1024;
    s.mem_avail_kb = (mem_total_mb - mem_used_mb) * 1024;
    s.disk_total_kb = disk_total_gb * 1048576;
    s.disk_avail_kb = (disk_total_gb - disk_used_gb) * 1048576;
    s.process_rss_kb = rss_mb * 1024;
    s.containers = static_cast<int>(containers);
    return s;
}

}  // namespace hivemind
