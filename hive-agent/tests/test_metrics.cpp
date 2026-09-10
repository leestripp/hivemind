/**
 * @file test_metrics.cpp
 * @brief Unit tests for the Metrics class
 */

#include <iostream>
#include <thread>
#include <chrono>

#include "core/Metrics.h"

using namespace hive;
using namespace std::chrono_literals;

static int failures = 0;
static int test_count = 0;

#define TEST(name) int test_##name()

#define CHECK(cond) do { \
    test_count++; \
    if (!(cond)) { \
        std::cerr << "FAIL: " << #cond << " at " << __FILE__ << ":" << __LINE__ << std::endl; \
        failures++; \
        return 1; \
    } \
} while(0)

#define CHECK_EQ(a, b) do { \
    test_count++; \
    if ((a) != (b)) { \
        std::cerr << "FAIL: " << #a << " == " << #b << " (" << (a) << " vs " << (b) << ") at " << __FILE__ << ":" << __LINE__ << std::endl; \
        failures++; \
        return 1; \
    } \
} while(0)

// ============================================================================
// Test: singleton
// ============================================================================
TEST(singleton) {
    auto& a = Metrics::get_instance();
    auto& b = Metrics::get_instance();
    CHECK_EQ(&a, &b);
    return 0;
}

// ============================================================================
// Test: increment counter
// ============================================================================
TEST(increment_counter) {
    auto& m = Metrics::get_instance();
    m.reset();

    m.increment("api_call", "test_op", true);
    m.increment("api_call", "test_op", true);
    m.increment("api_call", "test_op", false);

    auto snap = m.snapshot();
    auto it = snap.counters.find("api_call");
    CHECK(it != snap.counters.end());
    auto nit = it->second.find("test_op");
    CHECK(nit != it->second.end());
    CHECK_EQ(nit->second.total, 3U);
    CHECK_EQ(nit->second.successes, 2U);
    CHECK_EQ(nit->second.failures, 1U);
    return 0;
}

// ============================================================================
// Test: record duration
// ============================================================================
TEST(record_duration) {
    auto& m = Metrics::get_instance();
    m.reset();

    m.record_duration("tool", "bash", 100.0, true);
    m.record_duration("tool", "bash", 200.0, true);
    m.record_duration("tool", "bash", 50.0, false);

    auto snap = m.snapshot();
    auto& counter = snap.counters["tool"]["bash"];
    CHECK_EQ(counter.total, 3U);
    CHECK_EQ(counter.successes, 2U);
    CHECK_EQ(counter.failures, 1U);
    CHECK(counter.avg_duration_ms > 100.0);  // average of 100, 200, 50 = 116.67
    CHECK_EQ(counter.max_duration_ms, 200.0);
    return 0;
}

// ============================================================================
// Test: ScopedTimer
// ============================================================================
TEST(scoped_timer) {
    auto& m = Metrics::get_instance();
    m.reset();

    {
        Metrics::ScopedTimer timer("test_cat", "test_name");
        std::this_thread::sleep_for(10ms);  // 10ms
    }  // timer records on destruction

    auto snap = m.snapshot();
    auto& counter = snap.counters["test_cat"]["test_name"];
    CHECK_EQ(counter.total, 1U);
    CHECK(counter.avg_duration_ms >= 5.0);  // Allow some scheduling slack
    CHECK(counter.avg_duration_ms < 500.0);  // But not too long
    return 0;
}

// ============================================================================
// Test: ScopedTimer with failure
// ============================================================================
TEST(scoped_timer_failure) {
    auto& m = Metrics::get_instance();
    m.reset();

    {
        Metrics::ScopedTimer timer("fail_cat", "fail_name");
        timer.set_success(false);
        std::this_thread::sleep_for(5ms);
    }

    auto snap = m.snapshot();
    auto& counter = snap.counters["fail_cat"]["fail_name"];
    CHECK_EQ(counter.total, 1U);
    CHECK_EQ(counter.successes, 0U);
    CHECK_EQ(counter.failures, 1U);
    return 0;
}

// ============================================================================
// Test: multiple categories
// ============================================================================
TEST(multiple_categories) {
    auto& m = Metrics::get_instance();
    m.reset();

    m.increment("api", "call1", true);
    m.increment("tool", "exec1", true);
    m.increment("mcp", "tool1", true);

    auto snap = m.snapshot();
    CHECK_EQ(snap.counters.size(), 3U);
    CHECK(snap.counters.count("api") > 0);
    CHECK(snap.counters.count("tool") > 0);
    CHECK(snap.counters.count("mcp") > 0);
    return 0;
}

// ============================================================================
// Test: total_operations
// ============================================================================
TEST(total_operations) {
    auto& m = Metrics::get_instance();
    m.reset();

    m.increment("a", "x", true);
    m.increment("a", "y", true);
    m.increment("b", "z", true);

    auto snap = m.snapshot();
    CHECK_EQ(snap.total_operations(), 3U);
    return 0;
}

// ============================================================================
// Test: reset clears everything
// ============================================================================
TEST(reset) {
    auto& m = Metrics::get_instance();

    m.increment("test", "x", true);
    CHECK(m.snapshot().total_operations() > 0);

    m.reset();
    CHECK_EQ(m.snapshot().total_operations(), 0U);
    return 0;
}

// ============================================================================
// Test: format_summary returns non-empty
// ============================================================================
TEST(format_summary) {
    auto& m = Metrics::get_instance();
    m.reset();

    m.increment("api_call", "test_endpoint", true);
    m.record_duration("api_call", "test_endpoint", 42.0, true);

    std::string summary = m.format_summary();
    CHECK(!summary.empty());
    CHECK(summary.find("api_call") != std::string::npos);
    CHECK(summary.find("test_endpoint") != std::string::npos);
    return 0;
}

// ============================================================================
// Test: thread safety (concurrent increments)
// ============================================================================
TEST(thread_safety) {
    auto& m = Metrics::get_instance();
    m.reset();

    constexpr int kThreads = 4;
    constexpr int kIncrementsPerThread = 100;

    std::vector<std::thread> threads;
    for (int i = 0; i < kThreads; ++i) {
        threads.emplace_back([&m]() {
            for (int j = 0; j < kIncrementsPerThread; ++j) {
                m.increment("concurrent", "op", true);
            }
        });
    }
    for (auto& t : threads) t.join();

    auto snap = m.snapshot();
    auto& counter = snap.counters["concurrent"]["op"];
    CHECK_EQ(counter.total, static_cast<size_t>(kThreads * kIncrementsPerThread));
    return 0;
}

// ============================================================================
// MAIN
// ============================================================================

int main() {
    std::cout << "Running Metrics tests..." << std::endl;

    int local_failures = 0;
    local_failures += test_singleton();
    local_failures += test_increment_counter();
    local_failures += test_record_duration();
    local_failures += test_scoped_timer();
    local_failures += test_scoped_timer_failure();
    local_failures += test_multiple_categories();
    local_failures += test_total_operations();
    local_failures += test_reset();
    local_failures += test_format_summary();
    local_failures += test_thread_safety();

    std::cout << "\nResults: " << (local_failures == 0 ? "PASS" : "FAIL") << std::endl;
    std::cout << "Total checks: " << test_count << std::endl;
    std::cout << "Failed checks: " << local_failures << std::endl;

    return local_failures > 0 ? 1 : 0;
}
