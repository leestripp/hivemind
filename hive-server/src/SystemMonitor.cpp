// hive-server/src/SystemMonitor.cpp — see SystemMonitor.h for the design.
#include "hivemind/SystemMonitor.h"

#include <array>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sys/statvfs.h>
#include <sys/utsname.h>
#include <unistd.h>

using nlohmann::json;

void SystemMonitor::start() {
    if (running_.exchange(true)) return;
    thread_ = std::thread([this] {
        while (running_) {
            Sample s = sample_once();
            {
                std::lock_guard lk(mu_);
                samples_.push_back(s);
                const int64_t cutoff = s.t - kRetainSec;
                while (!samples_.empty() && samples_.front().t < cutoff)
                    samples_.pop_front();
            }
            for (int i = 0; i < kIntervalSec * 10 && running_; ++i)
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    });
}

void SystemMonitor::stop() {
    if (!running_.exchange(false)) return;
    if (thread_.joinable()) thread_.join();
}

SystemMonitor::Sample SystemMonitor::sample_once() {
    Sample s;
    s.t = static_cast<int64_t>(time(nullptr));

    // ── CPU%: delta of the aggregate "cpu" line in /proc/stat ──
    if (std::ifstream in{"/proc/stat"}; in) {
        std::string cpu;
        uint64_t user=0, nice=0, sys=0, idle=0, iowait=0, irq=0, soft=0;
        in >> cpu >> user >> nice >> sys >> idle >> iowait >> irq >> soft;
        const uint64_t total = user+nice+sys+idle+iowait+irq+soft;
        const uint64_t idl   = idle + iowait;
        if (prev_total_ && total > prev_total_) {
            const double dt = double(total - prev_total_);
            const double di = double(idl - prev_idle_);
            s.cpu_pct = 100.0 * (1.0 - di / dt);
            if (s.cpu_pct < 0) s.cpu_pct = 0;
            if (s.cpu_pct > 100) s.cpu_pct = 100;
        }
        prev_total_ = total; prev_idle_ = idl;
    }

    // ── memory: /proc/meminfo ──
    if (std::ifstream in{"/proc/meminfo"}; in) {
        std::string key, unit; int64_t val;
        while (in >> key >> val >> unit) {
            if (key == "MemTotal:")     s.mem_total_kb = val;
            if (key == "MemAvailable:") s.mem_avail_kb = val;
            if (s.mem_total_kb && s.mem_avail_kb) break;
        }
    }

    // ── load average ──
    if (std::ifstream in{"/proc/loadavg"}; in) in >> s.load1;

    // ── / filesystem ──
    struct statvfs vfs{};
    if (statvfs("/", &vfs) == 0 && vfs.f_blocks) {
        s.disk_total_kb = int64_t(vfs.f_blocks) * vfs.f_frsize / 1024;
        s.disk_avail_kb = int64_t(vfs.f_bavail) * vfs.f_frsize / 1024;
        s.disk_pct = 100.0 * (1.0 - double(s.disk_avail_kb) / s.disk_total_kb);
    }

    // ── own RSS: /proc/self/status ──
    if (std::ifstream in{"/proc/self/status"}; in) {
        std::string line;
        while (std::getline(in, line))
            if (line.rfind("VmRSS:", 0) == 0) {
                s.self_rss_kb = std::strtoll(line.c_str() + 6, nullptr, 10);
                break;
            }
    }

    s.containers = count_containers();
    return s;
}

int SystemMonitor::count_containers() {
    if (!podman_checked_) {
        podman_ok_ = std::system("command -v podman >/dev/null 2>&1") == 0;
        podman_checked_ = true;
    }
    if (!podman_ok_) return 0;
    std::array<char, 64> buf{};
    std::string out;
    if (FILE* p = popen("podman ps --filter name=^hive- -q 2>/dev/null | wc -l", "r")) {
        while (fgets(buf.data(), int(buf.size()), p)) out += buf.data();
        pclose(p);
    }
    return out.empty() ? 0 : std::atoi(out.c_str());
}

json SystemMonitor::snapshot() {
    Sample s;
    {
        std::lock_guard lk(mu_);
        if (!samples_.empty()) s = samples_.back();
    }
    char host[256] = {0};
    gethostname(host, sizeof(host) - 1);
    struct utsname uts{};
    uname(&uts);
    const long cores = sysconf(_SC_NPROCESSORS_ONLN);
    double up_s = 0;
    if (std::ifstream in{"/proc/uptime"}; in) in >> up_s;

    const double mem_pct = s.mem_total_kb
        ? 100.0 * (1.0 - double(s.mem_avail_kb) / s.mem_total_kb) : 0;
    return json{
        {"host",    {{"hostname", host}, {"kernel", uts.release},
                     {"cores", cores}, {"uptime_s", int64_t(up_s)}}},
        {"now",     {{"t", s.t},
                     {"cpu_pct", s.cpu_pct}, {"load1", s.load1},
                     {"mem_total_mb", s.mem_total_kb / 1024},
                     {"mem_used_mb", (s.mem_total_kb - s.mem_avail_kb) / 1024},
                     {"mem_pct", mem_pct},
                     {"disk_total_gb", s.disk_total_kb / 1048576},
                     {"disk_used_gb", (s.disk_total_kb - s.disk_avail_kb) / 1048576},
                     {"disk_pct", s.disk_pct},
                     {"server_rss_mb", s.self_rss_kb / 1024},
                     {"containers", s.containers}}},
        {"history_samples", samples_.size()},
        {"interval_s", kIntervalSec},
    };
}

json SystemMonitor::history(int minutes) {
    if (minutes <= 0) minutes = 60;
    if (minutes > kRetainSec / 60) minutes = kRetainSec / 60;
    const int64_t cutoff = int64_t(time(nullptr)) - int64_t(minutes) * 60;
    json arr = json::array();
    std::lock_guard lk(mu_);
    for (auto& s : samples_) {
        if (s.t < cutoff) continue;
        const double mem_pct = s.mem_total_kb
            ? 100.0 * (1.0 - double(s.mem_avail_kb) / s.mem_total_kb) : 0;
        arr.push_back({{"t", s.t}, {"cpu", s.cpu_pct}, {"mem", mem_pct},
                       {"load1", s.load1}, {"disk", s.disk_pct},
                       {"rss_mb", s.self_rss_kb / 1024},
                       {"containers", s.containers}});
    }
    return json{{"minutes", minutes}, {"samples", arr}};
}
