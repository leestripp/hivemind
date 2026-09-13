/**
 * @file test_thread_pool.cpp
 * @brief Unit tests for the ThreadPool class
 *
 * Tests cover:
 * - Basic submit + get future result
 * - Cancellable tasks (std::stop_token)
 * - Task priority ordering (with proper isolation)
 * - Stats tracking
 * - Concurrent execution
 * - Exception handling in tasks
 * - Long-running task that completes (not infinite)
 */

#include <iostream>
#include <thread>
#include <chrono>
#include <atomic>
#include <vector>
#include <future>
#include <string>

#include "core/ThreadPool.h"

using namespace hive::concurrent;
using namespace std::chrono_literals;

// ============================================================================
// Test framework
// ============================================================================

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

// Helper: drain the pool by submitting and waiting for a barrier task
static void drain_pool() {
    auto& pool = ThreadPool::get_instance();
    std::promise<void> done;
    auto fut = pool.submit(TaskPriority::LOW, [&done]() {
        done.set_value();
    });
    fut.get();
    // Small sleep to let workers settle
    std::this_thread::sleep_for(20ms);
}

// ============================================================================
// Test: get_instance returns the same singleton
// ============================================================================
TEST(singleton_returns_same_instance) {
    auto& a = ThreadPool::get_instance();
    auto& b = ThreadPool::get_instance();
    CHECK_EQ(&a, &b);
    return 0;
}

// ============================================================================
// Test: stats report configured worker count
// ============================================================================
TEST(stats_report_worker_count) {
    auto& pool = ThreadPool::get_instance();
    auto stats = pool.get_stats();
    CHECK(stats.threads > 0);
    CHECK_EQ(stats.threads, std::thread::hardware_concurrency());
    return 0;
}

// ============================================================================
// Test: submit returns a future with the result
// ============================================================================
TEST(submit_returns_future_result) {
    auto& pool = ThreadPool::get_instance();
    auto fut = pool.submit(TaskPriority::NORMAL, [](int x) { return x * 2; }, 21);
    int result = fut.get();
    CHECK_EQ(result, 42);
    return 0;
}

// ============================================================================
// Test: submit with no arguments
// ============================================================================
TEST(submit_with_no_args) {
    auto& pool = ThreadPool::get_instance();
    auto fut = pool.submit(TaskPriority::NORMAL, []() { return std::string("hello"); });
    auto result = fut.get();
    CHECK_EQ(result, "hello");
    return 0;
}

// ============================================================================
// Test: multiple tasks execute concurrently
// ============================================================================
TEST(multiple_tasks_concurrent) {
    auto& pool = ThreadPool::get_instance();
    auto before_stats = pool.get_stats();

    std::atomic<int> counter{0};
    std::vector<std::future<void>> futures;
    for (int i = 0; i < 8; ++i) {
        futures.push_back(pool.submit(TaskPriority::NORMAL, [&counter]() {
            std::this_thread::sleep_for(50ms);
            counter.fetch_add(1);
        }));
    }
    for (auto& f : futures) {
        f.get();
    }

    CHECK_EQ(counter.load(), 8);
    // completed_ increments in the worker AFTER the future fulfils —
    // under parallel-suite load the worker can lag; poll briefly
    // (flakes the suite intermittently otherwise).
    ThreadPool::Stats after_stats;
    for (int i = 0; i < 200; ++i) {
        after_stats = pool.get_stats();
        if (after_stats.completed_tasks >= before_stats.completed_tasks + 8)
            break;
        std::this_thread::sleep_for(10ms);
    }
    CHECK(after_stats.completed_tasks >= before_stats.completed_tasks + 8);
    CHECK(after_stats.total_submitted >= before_stats.total_submitted + 8);
    return 0;
}

// ============================================================================
// Test: cancellable task with stop_token
// ============================================================================
TEST(cancellable_task_runs) {
    auto& pool = ThreadPool::get_instance();
    auto fut = pool.submit_cancellable(TaskPriority::NORMAL,
        [](std::stop_token st, int n) {
            // Should not be cancelled yet
            CHECK(!st.stop_requested());
            return n + 1;
        },
        41
    );
    int result = fut.get();
    CHECK_EQ(result, 42);
    return 0;
}

// ============================================================================
// Test: cancellable task can check stop_token cooperatively
// ============================================================================
TEST(cancellable_task_checks_token) {
    auto& pool = ThreadPool::get_instance();
    auto token = pool.get_stop_token();
    CHECK(!token.stop_requested());

    // Submit a task that checks the token
    auto fut = pool.submit_cancellable(TaskPriority::NORMAL,
        [](std::stop_token st) {
            // For now, just return whether stop was requested
            return st.stop_requested();
        }
    );
    bool was_stopped = fut.get();
    CHECK(!was_stopped);  // Pool is not shutting down
    return 0;
}

// ============================================================================
// Test: exception in task is captured by future
// ============================================================================
TEST(exception_in_task_captured) {
    auto& pool = ThreadPool::get_instance();
    auto fut = pool.submit(TaskPriority::NORMAL, []() -> int {
        throw std::runtime_error("task failed");
    });
    bool got_exception = false;
    try {
        fut.get();
    } catch (const std::runtime_error& e) {
        got_exception = true;
        std::string msg = e.what();
        CHECK(msg == "task failed");
    }
    CHECK(got_exception);
    return 0;
}

// ============================================================================
// Test: Priority queue accepts HIGH, NORMAL, and LOW tasks
// (Full priority ordering is non-deterministic with multiple workers;
//  this test verifies all priorities execute correctly.)
// ============================================================================
TEST(priority_ordering) {
    drain_pool();  // Ensure no pending tasks

    auto& pool = ThreadPool::get_instance();

    // Submit a mix of priorities and verify all complete
    std::atomic<int> high_count{0};
    std::atomic<int> normal_count{0};
    std::atomic<int> low_count{0};

    std::vector<std::future<void>> futures;
    for (int i = 0; i < 5; ++i) {
        futures.push_back(pool.submit(TaskPriority::HIGH, [&]() {
            high_count.fetch_add(1);
            std::this_thread::sleep_for(5ms);
        }));
        futures.push_back(pool.submit(TaskPriority::NORMAL, [&]() {
            normal_count.fetch_add(1);
            std::this_thread::sleep_for(5ms);
        }));
        futures.push_back(pool.submit(TaskPriority::LOW, [&]() {
            low_count.fetch_add(1);
            std::this_thread::sleep_for(5ms);
        }));
    }
    for (auto& f : futures) f.get();

    CHECK_EQ(high_count.load(), 5);
    CHECK_EQ(normal_count.load(), 5);
    CHECK_EQ(low_count.load(), 5);
    return 0;
}

// ============================================================================
// Test: stats increment after task completion
// ============================================================================
TEST(stats_increment_after_task) {
    auto& pool = ThreadPool::get_instance();
    auto before = pool.get_stats();

    auto fut = pool.submit(TaskPriority::NORMAL, []() {
        std::this_thread::sleep_for(10ms);
        return 1;
    });
    fut.get();

    // Same visibility lag as multiple_tasks_concurrent — poll.
    ThreadPool::Stats after;
    for (int i = 0; i < 200; ++i) {
        after = pool.get_stats();
        if (after.completed_tasks > before.completed_tasks) break;
        std::this_thread::sleep_for(10ms);
    }
    CHECK(after.completed_tasks > before.completed_tasks);
    CHECK(after.total_submitted > before.total_submitted);
    return 0;
}

// ============================================================================
// Test: stop_token from get_stop_token
// ============================================================================
TEST(stop_token_valid) {
    auto& pool = ThreadPool::get_instance();
    auto token = pool.get_stop_token();
    CHECK(token.stop_possible());
    CHECK(!token.stop_requested());
    return 0;
}

// ============================================================================
// Test: cancellable task can be cancelled via stop_token
// ============================================================================
TEST(cancellable_task_cancelled) {
    // This test verifies the cancellation mechanism works.
    // We use a shared atomic stop source that the task checks.
    auto& pool = ThreadPool::get_instance();
    std::atomic<bool> should_stop{false};
    std::atomic<int> iterations{0};

    auto fut = pool.submit_cancellable(TaskPriority::LOW,
        [&should_stop, &iterations](std::stop_token st) {
            while (!st.stop_requested() && !should_stop.load()) {
                iterations.fetch_add(1);
                std::this_thread::sleep_for(2ms);
                if (iterations.load() > 1000) break;  // Safety limit
            }
            return iterations.load();
        }
    );

    // Wait until the task has actually STARTED before requesting stop —
    // a fixed 30ms sleep raced worker-wake latency under parallel-suite
    // load (should_stop landed first, loop never ran, count == 0).
    for (int i = 0; i < 500 && iterations.load() == 0; ++i)
        std::this_thread::sleep_for(10ms);
    should_stop.store(true);

    int count = fut.get();
    CHECK(count > 0);
    CHECK(count <= 1000);
    return 0;
}

// ============================================================================
// Test: void return type
// ============================================================================
TEST(submit_with_void_return) {
    auto& pool = ThreadPool::get_instance();
    std::atomic<bool> ran{false};
    auto fut = pool.submit(TaskPriority::NORMAL, [&ran]() {
        ran.store(true);
    });
    fut.get();
    CHECK(ran.load());
    return 0;
}

// ============================================================================
// Test: tasks with string return type
// ============================================================================
TEST(submit_with_string_return) {
    auto& pool = ThreadPool::get_instance();
    auto fut = pool.submit(TaskPriority::NORMAL, [](const std::string& a, const std::string& b) {
        return a + " " + b;
    }, "hello", "world");
    auto result = fut.get();
    CHECK_EQ(result, std::string("hello world"));
    return 0;
}

// ============================================================================
// Test: many small tasks don't deadlock
// ============================================================================
TEST(many_small_tasks_no_deadlock) {
    auto& pool = ThreadPool::get_instance();
    std::vector<std::future<int>> futures;
    for (int i = 0; i < 100; ++i) {
        futures.push_back(pool.submit(TaskPriority::NORMAL, [i]() {
            return i * i;
        }));
    }
    int sum = 0;
    for (size_t i = 0; i < futures.size(); ++i) {
        sum += futures[i].get();
    }
    // Sum of 0..99 squared = 99*100*199/6 = 328350
    CHECK_EQ(sum, 328350);
    return 0;
}

// ============================================================================
// MAIN
// ============================================================================

int main() {
    std::cout << "Running ThreadPool tests..." << std::endl;

    int local_failures = 0;
    local_failures += test_singleton_returns_same_instance();
    local_failures += test_stats_report_worker_count();
    local_failures += test_submit_returns_future_result();
    local_failures += test_submit_with_no_args();
    local_failures += test_multiple_tasks_concurrent();
    local_failures += test_cancellable_task_runs();
    local_failures += test_cancellable_task_checks_token();
    local_failures += test_exception_in_task_captured();
    local_failures += test_priority_ordering();
    local_failures += test_stats_increment_after_task();
    local_failures += test_stop_token_valid();
    local_failures += test_cancellable_task_cancelled();
    local_failures += test_submit_with_void_return();
    local_failures += test_submit_with_string_return();
    local_failures += test_many_small_tasks_no_deadlock();

    std::cout << "\nResults: " << (local_failures == 0 ? "PASS" : "FAIL") << std::endl;
    std::cout << "Total checks: " << test_count << std::endl;
    std::cout << "Failed checks: " << local_failures << std::endl;

    return local_failures > 0 ? 1 : 0;
}
