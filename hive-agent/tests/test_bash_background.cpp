/**
 * @file test_bash_background.cpp
 * @brief Integration tests for execute_bash background mode
 */

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include <filesystem>
#include <thread>
#include <chrono>

#include "tools/BashTool.h"
#include "tools/ToolHandler.h"
#include "core/FileIO.h"
#include "core/BackgroundProcessManager.h"

using json = nlohmann::json;

static FileIO make_test_file_io() {
    auto tmp = std::filesystem::temp_directory_path() / "daedalus_bash_bg_test";
    std::filesystem::create_directories(tmp);
    return FileIO(tmp);
}

TEST_CASE("BashTool background mode starts a detached process") {
    ToolHandler::set_auto_mode(true);
    auto file_io = make_test_file_io();
    BashTool tool;

    json args = {
        {"command", "sleep 1"},
        {"background", true}
    };

    auto result = tool.execute(args, file_io);

    CHECK(result["status"] == "background");
    CHECK(result.contains("process_id"));
    CHECK(result.contains("pid"));
    CHECK(result["pid"].get<int>() > 0);
    CHECK(result["command"] == "sleep 1");

    std::string process_id = result["process_id"].get<std::string>();

    // Process should still be running immediately after start
    auto& manager = BackgroundProcessManager::get_instance();
    auto info = manager.get_process_info(process_id);
    REQUIRE(info.has_value());
    CHECK(info->exited == false);
}

TEST_CASE("BashTool auto-detects trailing & as background intent") {
    ToolHandler::set_auto_mode(true);
    auto file_io = make_test_file_io();
    BashTool tool;

    json args = {
        {"command", "sleep 0.3 &"}
    };

    auto result = tool.execute(args, file_io);

    CHECK(result["status"] == "background");
    CHECK(result["command"] == "sleep 0.3");
    CHECK(result.contains("process_id"));

    std::string process_id = result["process_id"].get<std::string>();

    // Poll until exit
    auto& manager = BackgroundProcessManager::get_instance();
    bool exited = false;
    for (int i = 0; i < 50 && !exited; ++i) {
        auto out = manager.poll_output(process_id, false);
        exited = out.exited;
        if (!exited) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }
    CHECK(exited == true);
}

TEST_CASE("BashTool can stop a background process") {
    ToolHandler::set_auto_mode(true);
    auto file_io = make_test_file_io();
    BashTool tool;

    json start_args = {
        {"command", "sleep 30"},
        {"action", "start"}
    };
    auto start_result = tool.execute(start_args, file_io);
    CHECK(start_result["status"] == "background");
    std::string process_id = start_result["process_id"].get<std::string>();

    json stop_args = {
        {"action", "stop"},
        {"process_id", process_id}
    };
    auto stop_result = tool.execute(stop_args, file_io);
    CHECK(stop_result["status"] == "success");

    // Verify the process is gone
    auto& manager = BackgroundProcessManager::get_instance();
    CHECK(manager.get_process_info(process_id).has_value() == false);
}

TEST_CASE("BashTool background poll reads stdout") {
    ToolHandler::set_auto_mode(true);
    auto file_io = make_test_file_io();
    BashTool tool;

    // Start a command that writes to stdout and then sleeps
    json start_args = {
        {"command", "echo hello_bg && sleep 30"},
        {"action", "start"}
    };
    auto start_result = tool.execute(start_args, file_io);
    CHECK(start_result["status"] == "background");
    std::string process_id = start_result["process_id"].get<std::string>();

    // Poll until we see output
    json poll_args = {
        {"action", "poll"},
        {"process_id", process_id}
    };

    bool found = false;
    for (int i = 0; i < 50 && !found; ++i) {
        auto poll_result = tool.execute(poll_args, file_io);
        if (poll_result.contains("stdout")) {
            std::string stdout_output = poll_result["stdout"].get<std::string>();
            found = stdout_output.find("hello_bg") != std::string::npos;
        }
        if (!found) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }
    CHECK(found == true);

    // Stop
    json stop_args = {
        {"action", "stop"},
        {"process_id", process_id}
    };
    tool.execute(stop_args, file_io);
}

TEST_CASE("BashTool synchronous mode still works") {
    ToolHandler::set_auto_mode(true);
    auto file_io = make_test_file_io();
    BashTool tool;

    json args = {
        {"command", "echo hello sync"}
    };

    auto result = tool.execute(args, file_io);

    CHECK(result["status"] == "success");
    CHECK(result["exit_code"] == 0);
    CHECK(result["output"].get<std::string>().find("hello sync") != std::string::npos);
}
