/**
 * @file ThreadPool.h
 * @brief Global thread pool for async operations
 *
 * A singleton thread pool that:
 * - Uses std::jthread with std::stop_token for cancellable work
 * - Supports task priorities (HIGH/NORMAL/LOW)
 * - Tracks stats (active, completed, queued)
 * - Gracefully shuts down with timeout
 *
 * Why this exists:
 * - Replaces ad-hoc std::async(launch::async, ...) which creates a fresh
 *   thread per call (footgun, no thread reuse, no cancellation).
 * - Provides a shared pool for async I/O (OmniNet HTTP/MCP, file ops).
 * - Centralizes thread management for cleaner shutdown.
 *
 * Thread-safety: All public methods are safe to call from any thread.
 */

#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <queue>
#include <stdexcept>
#include <stop_token>
#include <thread>
#include <type_traits>
#include <vector>

namespace hive::concurrent {

/**
 * @brief Task priority levels
 *
 * Higher priority tasks are dequeued first. Use sparingly - the pool
 * is meant for I/O-bound work, not compute-heavy parallelism.
 */
enum class TaskPriority {
    HIGH = 0,    ///< User-facing (API calls, ESC response, UI updates)
    NORMAL = 1,  ///< Tool execution, file I/O, MCP calls
    LOW = 2      ///< Cleanup, log rotation, background polling
};

/**
 * @brief Global thread pool singleton
 *
 * Usage:
 * @code
 *   auto& pool = ThreadPool::get_instance();
 *
 *   // Submit a regular task
 *   auto fut = pool.submit(TaskPriority::NORMAL, [](int x) { return x * 2; }, 21);
 *   int result = fut.get();  // 42
 *
 *   // Submit a cancellable task (must take std::stop_token as first arg)
 *   auto fut2 = pool.submit_cancellable(TaskPriority::NORMAL,
 *       [](std::stop_token st, const std::string& url) {
 *           // Periodically check st.stop_requested() and bail out
 *           if (st.stop_requested()) return std::string{};
 *           return download(url);
 *       },
 *       std::string("https://example.com")
 *   );
 * @endcode
 */
class ThreadPool {
public:
    /**
     * @brief Pool statistics
     */
    struct Stats {
        size_t threads;          ///< Number of worker threads
        size_t queued_tasks;     ///< Tasks waiting in the queue
        size_t completed_tasks;  ///< Total tasks completed (cumulative)
        size_t active_tasks;     ///< Tasks currently executing
        size_t total_submitted;  ///< Total tasks ever submitted (cumulative)
    };

    /**
     * @brief Get the global pool instance
     */
    static ThreadPool& get_instance();

    // Non-copyable, non-movable singleton
    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    /**
     * @brief Configure the number of worker threads
     *
     * MUST be called before any submit() calls, or after shutdown().
     * Default is std::thread::hardware_concurrency() at first use.
     *
     * @param num_threads Desired worker count (0 = use hardware_concurrency)
     */
    void configure(size_t num_threads);

    /**
     * @brief Submit a task and get a future to its result
     *
     * @tparam F Callable type
     * @tparam Args Argument types
     * @param priority Task priority
     * @param f Callable
     * @param args Arguments to f
     * @return std::future to the callable's return value
     */
    template <typename F, typename... Args>
    auto submit(TaskPriority priority, F&& f, Args&&... args)
        -> std::future<std::invoke_result_t<F, Args...>>;

    /**
     * @brief Submit a cancellable task (F must take std::stop_token as first arg)
     *
     * The task can check stop_token.stop_requested() to cooperate with
     * cancellation. Note: std::stop_token does NOT interrupt blocking I/O -
     * the task must poll the token itself.
     *
     * @tparam F Callable type, signature F(std::stop_token, Args...)
     * @tparam Args Argument types
     * @param priority Task priority
     * @param f Callable (first arg must be std::stop_token)
     * @param args Remaining arguments
     * @return std::future to the callable's return value
     */
    template <typename F, typename... Args>
    auto submit_cancellable(TaskPriority priority, F&& f, Args&&... args)
        -> std::future<std::invoke_result_t<F, std::stop_token, Args...>>;

    /**
     * @brief Get current pool statistics
     */
    Stats get_stats() const;

    /**
     * @brief Graceful shutdown
     *
     * Signals all workers to stop, waits up to @p timeout for them to finish
     * the current task, then forces shutdown. Safe to call multiple times.
     *
     * @param timeout Maximum wait time for graceful drain
     */
    void shutdown(std::chrono::milliseconds timeout = std::chrono::seconds(5));

    /**
     * @brief Get a stop_token for the pool's lifetime
     *
     * Tasks can hold this token to know when the pool is shutting down.
     */
    std::stop_token get_stop_token() const { return stop_source_.get_token(); }

    /**
     * @brief Destructor - calls shutdown() with default timeout
     */
    ~ThreadPool();

private:
    ThreadPool();
    void worker_loop(std::stop_token st);

    struct Task {
        std::function<void(std::stop_token)> work;
        TaskPriority priority;
        std::chrono::steady_clock::time_point submitted_at;

        // For priority_queue: lower enum value = higher priority = dequeued first
        bool operator<(const Task& other) const {
            return static_cast<int>(priority) > static_cast<int>(other.priority);
        }
    };

    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::priority_queue<Task> tasks_;
    std::vector<std::jthread> workers_;
    std::atomic<bool> stop_{false};
    std::atomic<bool> configured_{false};
    std::atomic<size_t> completed_{0};
    std::atomic<size_t> active_{0};
    std::atomic<size_t> submitted_{0};

    std::stop_source stop_source_;
};

// =============================================================================
// Template implementations (must be in header for type deduction)
// =============================================================================

template <typename F, typename... Args>
auto ThreadPool::submit(TaskPriority priority, F&& f, Args&&... args)
    -> std::future<std::invoke_result_t<F, Args...>> {

    using ReturnType = std::invoke_result_t<F, Args...>;

    // §fix-threadpool-shutdown (2026-07-18): std::packaged_task has no
    // set_exception() — use std::promise on the stopped-pool fast path
    // so the returned future resolves with the exception instead of
    // blocking forever (worker pool will never run the task).
    auto make_exception_future = [](std::exception_ptr eptr) {
        std::promise<ReturnType> p;
        p.set_exception(std::move(eptr));
        return p.get_future();
    };

    // Wrap the callable in a shared_ptr so the packaged_task can be copied
    auto task_ptr = std::make_shared<std::packaged_task<ReturnType()>>(
        std::bind(std::forward<F>(f), std::forward<Args>(args)...)
    );

    std::future<ReturnType> result = task_ptr->get_future();

    {
        std::unique_lock<std::mutex> lock(mutex_);
        if (stop_.load()) {
            // §fix (2026-07-18): pool is stopped — no worker will ever
            // run this task, so future.get() would block forever.
            // Return a future holding an exception instead of enqueueing.
            return make_exception_future(std::make_exception_ptr(
                std::runtime_error("ThreadPool: submit() after shutdown")));
        }

        Task task;
        task.work = [task_ptr](std::stop_token /*st*/) {
            (*task_ptr)();
        };
        task.priority = priority;
        task.submitted_at = std::chrono::steady_clock::now();
        tasks_.push(std::move(task));
        submitted_.fetch_add(1, std::memory_order_relaxed);
    }

    cv_.notify_one();
    return result;
}

template <typename F, typename... Args>
auto ThreadPool::submit_cancellable(TaskPriority priority, F&& f, Args&&... args)
    -> std::future<std::invoke_result_t<F, std::stop_token, Args...>> {

    using ReturnType = std::invoke_result_t<F, std::stop_token, Args...>;

    auto task_ptr = std::make_shared<std::packaged_task<ReturnType()>>(
        [fn = std::forward<F>(f),
         tup = std::tuple<std::decay_t<Args>...>(std::forward<Args>(args)...),
         stop_token = stop_source_.get_token()]() mutable -> ReturnType {
            return std::apply(
                [&fn, stop_token](auto&&... unpacked_args) -> ReturnType {
                    return fn(stop_token, std::forward<decltype(unpacked_args)>(unpacked_args)...);
                },
                std::move(tup)
            );
        }
    );

    std::future<ReturnType> result = task_ptr->get_future();

    {
        std::unique_lock<std::mutex> lock(mutex_);
        Task task;
        task.work = [task_ptr](std::stop_token /*st*/) {
            (*task_ptr)();
        };
        task.priority = priority;
        task.submitted_at = std::chrono::steady_clock::now();
        tasks_.push(std::move(task));
        submitted_.fetch_add(1, std::memory_order_relaxed);
    }

    cv_.notify_one();
    return result;
}

} // namespace hive::concurrent
