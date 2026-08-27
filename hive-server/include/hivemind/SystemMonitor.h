#pragma once

#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <atomic>
#include <thread>
#include <nlohmann/json.hpp>

/**
 * SystemMonitor — lightweight host metrics sampler for the web UI System
 * page. A single thread reads /proc every kIntervalSec (microseconds of
 * work; no external processes except one `podman ps` count per sample) and
 * appends to an in-memory ring buffer (kRetainSec of history, ~6 kB).
 *
 * Design answer to "background service or on-demand?": neither extreme.
 * The sampler always runs — it's cheap and gives the page real history —
 * while the web page only POLLS while it is the active page. No SQLite,
 * no separate service, nothing survives a restart (fine for a dashboard).
 */
class SystemMonitor {
public:
    struct Sample {
        int64_t t = 0;              // epoch seconds (UTC)
        double  cpu_pct = 0;        // 0–100, all cores (delta of /proc/stat)
        double  load1 = 0;
        int64_t mem_total_kb = 0;
        int64_t mem_avail_kb = 0;
        double  disk_pct = 0;       // usage of the / filesystem
        int64_t disk_total_kb = 0;
        int64_t disk_avail_kb = 0;
        int64_t self_rss_kb = 0;    // hive-server process resident memory
        int     containers = 0;     // running hive-* podman containers
    };

    SystemMonitor() = default;
    ~SystemMonitor() { stop(); }

    void start();
    void stop();

    /// Current sample + static host facts (hostname, cores, boot time).
    nlohmann::json snapshot();
    /// Samples from the last `minutes`, oldest first (capped at retention).
    nlohmann::json history(int minutes);

    static constexpr int kIntervalSec = 15;
    static constexpr int kRetainSec   = 24 * 3600;   // 24 h ≈ 5760 samples

private:
    Sample sample_once();
    int    count_containers();      // 0 when podman absent

    std::thread       thread_;
    std::atomic<bool> running_{false};
    mutable std::mutex mu_;
    std::deque<Sample> samples_;

    // /proc/stat cursors for CPU% deltas
    uint64_t prev_total_ = 0, prev_idle_ = 0;
    bool     podman_checked_ = false, podman_ok_ = false;
};
