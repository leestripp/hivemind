/**
 * @file Metrics.h
 * @brief Lightweight metrics collection for hive-brain
 *
 * Tracks:
 * - API calls (count, total time, average time)
 * - Tool executions (count, by name, success/failure)
 * - MCP tool calls
 * - Compression calls
 * - Network requests via HttpSession
 *
 * Thread-safe: All methods can be called from any thread.
 *
 * Usage:
 * @code
 *   auto& m = Metrics::get_instance();
 *   {
 *     auto _ = m.ScopedTimer("api_call");
 *     // ... do work ...
 *   }  // timer automatically records duration
 *
 *   m.increment("tool_execute", "execute_bash", true);  // success
 *   m.increment("tool_execute", "execute_bash", false); // failure
 *
 *   std::cout << m.format_summary();  // human-readable
 * @endcode
 */

#pragma once

#include <atomic>
#include <chrono>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace hive {

/**
 * @brief Global metrics singleton
 */
class Metrics {
public:
    static Metrics& get_instance();

    /**
     * @brief Increment a counter
     * @param category Counter category (e.g., "api_call", "tool_execute")
     * @param name Counter name (e.g., "execute_bash", "make_api_request")
     * @param success true for success, false for failure
     */
    void increment(const std::string& category,
                   const std::string& name,
                   bool success = true);

    /**
     * @brief Record a duration for an operation
     * @param category Counter category
     * @param name Counter name
     * @param duration_ms Duration in milliseconds
     * @param success true for success
     */
    void record_duration(const std::string& category,
                         const std::string& name,
                         double duration_ms,
                         bool success = true);

    /**
     * @brief RAII timer that records on destruction
     */
    class ScopedTimer {
    public:
        ScopedTimer(std::string category, std::string name, bool success = true)
            : category_(std::move(category)),
              name_(std::move(name)),
              success_(success),
              start_(std::chrono::steady_clock::now()) {}

        ~ScopedTimer() {
            auto end = std::chrono::steady_clock::now();
            double ms = std::chrono::duration<double, std::milli>(end - start_).count();
            Metrics::get_instance().record_duration(category_, name_, ms, success_);
        }

        void set_success(bool s) { success_ = s; }

    private:
        std::string category_;
        std::string name_;
        bool success_;
        std::chrono::steady_clock::time_point start_;
    };

    /**
     * @brief Get current metrics snapshot
     */
    struct Snapshot {
        // Per-category, per-name counts
        struct Counter {
            size_t total = 0;
            size_t successes = 0;
            size_t failures = 0;
            double total_duration_ms = 0.0;
            double avg_duration_ms = 0.0;
            double max_duration_ms = 0.0;
        };
        std::map<std::string, std::map<std::string, Counter>> counters;

        // Aggregate
        size_t total_operations() const;
    };

    Snapshot snapshot() const;

    /**
     * @brief Format snapshot as human-readable string
     */
    std::string format_summary() const;

    /**
     * @brief Reset all metrics
     */
    void reset();

private:
    Metrics() = default;
    mutable std::mutex mutex_;
    std::map<std::string, std::map<std::string, Snapshot::Counter>> counters_;
};

} // namespace hive
