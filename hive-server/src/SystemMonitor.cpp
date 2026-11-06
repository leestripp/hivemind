// hive-server/src/SystemMonitor.cpp — see SystemMonitor.h for the design.
#include "hivemind/SystemMonitor.h"

using nlohmann::json;

void SystemMonitor::start() {
    if (running_.exchange(true)) return;
    thread_ = std::thread([this] {
        while (running_) {
            Sample s = sampler_.sample();
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

json SystemMonitor::snapshot() {
    Sample s;
    size_t history_samples = 0;
    {
        std::lock_guard lk(mu_);
        if (!samples_.empty()) s = samples_.back();
        history_samples = samples_.size();
    }
    auto now = hivemind::host_now_json(s);
    // Preserve the original hub API field alongside the shared rss_mb name.
    now["server_rss_mb"] = s.process_rss_kb / 1024;
    return json{
        {"host", hivemind::host_facts_json()},
        {"now", std::move(now)},
        {"history_samples", history_samples},
        {"interval_s", kIntervalSec},
    };
}

json SystemMonitor::history(int minutes) {
    if (minutes <= 0) minutes = 60;
    if (minutes > kRetainSec / 60) minutes = kRetainSec / 60;
    const int64_t cutoff = static_cast<int64_t>(time(nullptr)) -
                           static_cast<int64_t>(minutes) * 60;
    json arr = json::array();
    std::lock_guard lk(mu_);
    for (const auto& s : samples_) {
        if (s.t >= cutoff) arr.push_back(hivemind::host_history_json(s));
    }
    return json{{"minutes", minutes}, {"samples", std::move(arr)}};
}
