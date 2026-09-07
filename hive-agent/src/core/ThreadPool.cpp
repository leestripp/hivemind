/**
 * @file ThreadPool.cpp
 * @brief Thread pool implementation
 */

#include "ThreadPool.h"

#include <iostream>

#include "core/Logger.h"

// §Phase 5.1: helper macro that converts LOG_CERR(INFO,  ...); style
// statements into LOG_INFO/LOG_WARNING/LOG_ERROR calls via the
// existing Logger. Same formatted text routed through the Logger
// instead of writing to stderr directly.
#define LOG_CERR(level, expr) do {                          \
        std::ostringstream _hive_msg;                   \
        _hive_msg << expr;                               \
        LOG_##level(_hive_msg.str());                    \
    } while (0)
namespace hive::concurrent {

ThreadPool& ThreadPool::get_instance() {
    static ThreadPool instance;
    return instance;
}

ThreadPool::ThreadPool() {
    // Default to hardware concurrency
    configure(std::thread::hardware_concurrency());
}

ThreadPool::~ThreadPool() {
    shutdown();
}

void ThreadPool::configure(size_t num_threads) {
    std::unique_lock<std::mutex> lock(mutex_);

    if (configured_.load()) {
        LOG_CERR(INFO,  "[ThreadPool] Warning: configure() called after workers started. "
                  << "Use shutdown() first to reconfigure.\n");
        return;
    }

    if (num_threads == 0) {
        num_threads = std::thread::hardware_concurrency();
    }
    if (num_threads == 0) {
        num_threads = 2;  // Fallback
    }

    workers_.reserve(num_threads);
    for (size_t i = 0; i < num_threads; ++i) {
        workers_.emplace_back([this](std::stop_token st) {
            worker_loop(st);
        });
    }
    configured_.store(true);
}

void ThreadPool::worker_loop(std::stop_token st) {
    while (!st.stop_requested() && !stop_.load()) {
        Task task;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock, [this, &st] {
                return stop_.load() || st.stop_requested() || !tasks_.empty();
            });

            if ((stop_.load() || st.stop_requested()) && tasks_.empty()) {
                return;
            }
            if (tasks_.empty()) {
                continue;
            }

            task = std::move(const_cast<Task&>(tasks_.top()));
            tasks_.pop();
        }

        active_.fetch_add(1, std::memory_order_relaxed);
        try {
            task.work(st);
        } catch (const std::exception& e) {
            LOG_CERR(INFO,  "[ThreadPool] Task threw exception: " << e.what());
        } catch (...) {
            LOG_CERR(INFO,  "[ThreadPool] Task threw unknown exception\n");
        }
        active_.fetch_sub(1, std::memory_order_relaxed);
        completed_.fetch_add(1, std::memory_order_relaxed);
    }
}

ThreadPool::Stats ThreadPool::get_stats() const {
    std::scoped_lock lock(mutex_);
    return Stats{
        .threads = workers_.size(),
        .queued_tasks = tasks_.size(),
        .completed_tasks = completed_.load(std::memory_order_relaxed),
        .active_tasks = active_.load(std::memory_order_relaxed),
        .total_submitted = submitted_.load(std::memory_order_relaxed),
    };
}

void ThreadPool::shutdown(std::chrono::milliseconds timeout) {
    {
        std::scoped_lock lock(mutex_);
        if (stop_.load()) {
            return;  // Already shutting down
        }
        stop_.store(true);
    }

    // Signal stop token so cancellable tasks can bail
    stop_source_.request_stop();

    // Wake all workers
    cv_.notify_all();

    // Request stop on each jthread (jthread auto-joins on destruction)
    for (auto& w : workers_) {
        if (w.joinable()) {
            w.request_stop();
        }
    }

    // Wait up to timeout for graceful drain
    auto deadline = std::chrono::steady_clock::now() + timeout;
    for (auto& w : workers_) {
        // Wait, but cap at deadline
        // jthread doesn't have a "join with timeout" so we just join
        // (timeout enforcement is best-effort via the stop signals above)
        if (w.joinable()) {
            w.join();
        }
        (void)deadline;  // Best-effort timeout; jthread::join() blocks
    }

    {
        std::scoped_lock lock(mutex_);
        workers_.clear();
    }
}

} // namespace hive::concurrent
