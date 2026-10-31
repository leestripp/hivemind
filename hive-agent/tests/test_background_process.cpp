/**
 * @file test_background_process.cpp
 * @brief Unit tests for BackgroundProcessManager functionality
 */

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include <core/BackgroundProcessManager.h>
#include <string>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <chrono>
#include <thread>

using namespace std::chrono_literals;

TEST_CASE("BackgroundProcessManager - start and stop simple command") {
    auto& manager = BackgroundProcessManager::get_instance();
    manager.cleanup_all();
    
    // Start a simple background process
    std::string pid = manager.start_background("sleep 5");
    CHECK(!pid.empty());
    
    // Verify process is not exited
    CHECK(!manager.has_exited(pid));
    
    // Get process info
    auto info = manager.get_process_info(pid);
    CHECK(info.has_value());
    CHECK(info->pid > 0);
    CHECK(info->command == "sleep 5");
    
    // Stop the process
    bool stopped = manager.stop_process(pid);
    CHECK(stopped);
    
    // Verify process is exited
    CHECK(manager.has_exited(pid));
}

TEST_CASE("BackgroundProcessManager - list active processes") {
    auto& manager = BackgroundProcessManager::get_instance();
    manager.cleanup_all();
    
    // Start a couple of processes
    std::string pid1 = manager.start_background("sleep 10");
    std::string pid2 = manager.start_background("sleep 10");
    
    CHECK(!pid1.empty());
    CHECK(!pid2.empty());
    CHECK(pid1 != pid2);
    
    // List active processes
    auto active = manager.list_active_processes();
    CHECK(active.size() >= 2);  // At least our 2 processes
    
    // Clean up
    manager.stop_process(pid1);
    manager.stop_process(pid2);
}

TEST_CASE("BackgroundProcessManager - poll output") {
    auto& manager = BackgroundProcessManager::get_instance();
    manager.cleanup_all();
    
    // Start a simple sleep process that produces no output
    std::string pid = manager.start_background("sleep 2");
    CHECK(!pid.empty());
    
    // Verify process is not exited immediately
    CHECK(!manager.has_exited(pid));
    
    // Poll output - should have empty stdout/stderr
    ProcessOutput output = manager.poll_output(pid);
    CHECK(output.stdout.empty());
    CHECK(output.stderr.empty());
    
    // Process should still be running (sleep 2 takes time)
    CHECK(!manager.has_exited(pid));
    
    // Stop the process
    bool stopped = manager.stop_process(pid);
    CHECK(stopped);
    
    // Verify process is exited after stop
    CHECK(manager.has_exited(pid));
}

TEST_CASE("BackgroundProcessManager - stop non-existent process") {
    auto& manager = BackgroundProcessManager::get_instance();
    manager.cleanup_all();
    
    // Try to stop a process that doesn't exist
    bool result = manager.stop_process("bg_99999");
    CHECK(result == false);  // Should return false for non-existent
    
    CHECK(manager.has_exited("bg_99999"));  // Non-existent = considered exited
}

TEST_CASE("BackgroundProcessManager - send to non-existent process") {
    auto& manager = BackgroundProcessManager::get_instance();
    manager.cleanup_all();
    
    SendResult result = manager.send_to_process("bg_99999", "test");
    CHECK(!result.success);
    CHECK(!result.error.empty());
}

TEST_CASE("BackgroundProcessManager - poll non-existent process") {
    auto& manager = BackgroundProcessManager::get_instance();
    manager.cleanup_all();
    
    ProcessOutput output = manager.poll_output("bg_99999");
    CHECK(output.stdout.empty());
    CHECK(output.stderr.empty());
    // Non-existent process returns empty output, exited flag depends on internal state
    // The key point is it doesn't crash
}

TEST_CASE("BackgroundProcessManager - process info") {
    auto& manager = BackgroundProcessManager::get_instance();
    manager.cleanup_all();
    
    // Start a process
    std::string pid = manager.start_background("echo test");
    CHECK(!pid.empty());
    
    // Get info while running
    auto info = manager.get_process_info(pid);
    CHECK(info.has_value());
    CHECK(info->process_id == pid);
    CHECK(info->pid > 0);
    CHECK(info->command == "echo test");
    CHECK(info->fifo_path.empty() == false);
    CHECK(info->stdout_path.empty() == false);
    CHECK(info->stderr_path.empty() == false);
    CHECK(info->exited == false);
    
    // Stop the process
    manager.stop_process(pid);
    
    // After stop, cleanup_process removes from map, so info may not be available
    auto info_after = manager.get_process_info(pid);
    // info_after may be nullopt since cleanup removes the entry
}

TEST_CASE("BackgroundProcessManager - cleanup all") {
    auto& manager = BackgroundProcessManager::get_instance();
    manager.cleanup_all();
    
    // Start multiple processes
    std::string pid1 = manager.start_background("sleep 100");
    std::string pid2 = manager.start_background("sleep 100");
    
    CHECK(!pid1.empty());
    CHECK(!pid2.empty());
    
    // Cleanup all
    manager.cleanup_all();
    
    // Verify all stopped
    auto active = manager.list_active_processes();
    CHECK(active.empty());
}
TEST_CASE("BackgroundProcessManager - natural exit notifies exactly once") {
    auto& manager = BackgroundProcessManager::get_instance();
    manager.cleanup_all();
    manager.set_completion_callback({});
    manager.set_current_origin("task-123", "thread-456");

    std::mutex mutex;
    std::condition_variable cv;
    int calls = 0;
    BackgroundProcessInfo completed_info;
    ProcessOutput completed_output;
    manager.set_completion_callback(
        [&](const BackgroundProcessInfo& info, const ProcessOutput& output) {
            std::lock_guard<std::mutex> lock(mutex);
            ++calls;
            completed_info = info;
            completed_output = output;
            cv.notify_all();
        });

    const std::string id = manager.start_background(
        "printf 'download complete'; printf 'warning' >&2; exit 7");
    REQUIRE(!id.empty());
    {
        std::unique_lock<std::mutex> lock(mutex);
        REQUIRE(cv.wait_for(lock, std::chrono::seconds(5),
                            [&] { return calls == 1; }));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(250));

    std::lock_guard<std::mutex> lock(mutex);
    CHECK(calls == 1);
    CHECK(completed_info.process_id == id);
    CHECK(completed_info.origin_task_id == "task-123");
    CHECK(completed_info.origin_thread_id == "thread-456");
    CHECK(completed_output.exited);
    CHECK(completed_output.exit_code == 7);
    CHECK(completed_output.stdout == "download complete");
    CHECK(completed_output.stderr == "warning");

    manager.set_completion_callback({});
    manager.cleanup_all();
}

TEST_CASE("BackgroundProcessManager - explicit stop does not notify") {
    auto& manager = BackgroundProcessManager::get_instance();
    manager.cleanup_all();
    std::atomic<int> calls{0};
    manager.set_completion_callback(
        [&](const BackgroundProcessInfo&, const ProcessOutput&) { ++calls; });
    const std::string id = manager.start_background("sleep 30");
    REQUIRE(!id.empty());
    CHECK(manager.stop_process(id));
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
    CHECK(calls.load() == 0);
    manager.set_completion_callback({});
    manager.cleanup_all();
}
