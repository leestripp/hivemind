/**
 * @file Metrics.cpp
 * @brief Lightweight metrics collection implementation
 */

#include "Metrics.h"

#include <iomanip>
#include <iostream>
#include <sstream>

namespace hive {

Metrics& Metrics::get_instance() {
    static Metrics instance;
    return instance;
}

void Metrics::increment(const std::string& category,
                       const std::string& name,
                       bool success) {
    std::scoped_lock lock(mutex_);
    auto& counter = counters_[category][name];
    counter.total++;
    if (success) counter.successes++;
    else counter.failures++;
}

void Metrics::record_duration(const std::string& category,
                             const std::string& name,
                             double duration_ms,
                             bool success) {
    std::scoped_lock lock(mutex_);
    auto& counter = counters_[category][name];
    counter.total++;
    if (success) counter.successes++;
    else counter.failures++;
    counter.total_duration_ms += duration_ms;
    counter.avg_duration_ms = counter.total_duration_ms / counter.total;
    if (duration_ms > counter.max_duration_ms) {
        counter.max_duration_ms = duration_ms;
    }
}

Metrics::Snapshot Metrics::snapshot() const {
    std::scoped_lock lock(mutex_);
    Snapshot s;
    s.counters = counters_;
    return s;
}

size_t Metrics::Snapshot::total_operations() const {
    size_t total = 0;
    for (const auto& [cat, names] : counters) {
        for (const auto& [name, counter] : names) {
            total += counter.total;
        }
    }
    return total;
}

std::string Metrics::format_summary() const {
    auto s = snapshot();
    std::ostringstream oss;

    if (s.counters.empty()) {
        return "(no metrics recorded)";
    }

    oss << "=== Metrics Summary ===\n";
    for (const auto& [cat, names] : s.counters) {
        oss << "\n[" << cat << "]\n";
        for (const auto& [name, counter] : names) {
            oss << "  " << name
                << ": " << counter.total << " calls"
                << " (" << counter.successes << " ok, " << counter.failures << " fail)";
            if (counter.total > 0) {
                oss << std::fixed << std::setprecision(1)
                    << " - avg " << counter.avg_duration_ms << "ms"
                    << ", max " << counter.max_duration_ms << "ms";
            }
            oss << "\n";
        }
    }
    return oss.str();
}

void Metrics::reset() {
    std::scoped_lock lock(mutex_);
    counters_.clear();
}

} // namespace hive
