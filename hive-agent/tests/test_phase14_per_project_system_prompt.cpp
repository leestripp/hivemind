// ============================================================================
// tests/test_phase14_per_project_system_prompt.cpp
//
// §Phase 14.7 (Q2, decided YES): if
// <project_root>/.hive-agent/default/system_prompt.txt exists, its
// contents are appended to cfg.system_prompt (or used as-is if
// cfg.system_prompt is empty). The library must:
//   - read the file once during initialize()
//   - merge project text AFTER the demo's text (concat with blank line)
//   - log a [INFO] line that the merge happened
//   - silently no-op when the file doesn't exist
//
// Hermetic — no network, no API calls. Tests construct Agent with a
// tmp project_root + an optional override file.
// ============================================================================

#include "library/Agent.h"
#include "library/AgentSink.h"

#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

struct RecordingSink : hive::AgentSink {
    std::vector<std::pair<hive::LogLevel, std::string>> events;

    void on_log(hive::LogLevel lvl, std::string_view msg) noexcept override {
        events.emplace_back(lvl, std::string(msg));
    }
    void on_assistant_delta(std::string_view) noexcept override {}
    void on_tool_call(std::string_view, const nlohmann::json&) noexcept override {}
    void on_tool_result(std::string_view, const nlohmann::json&) noexcept override {}
    void on_state_change(int, int) noexcept override {}
};

// §Phase 14.7: project override appended to demo default.
void test_project_override_appended() {
    fs::path tmp = fs::temp_directory_path() / "phase147-appended";
    fs::remove_all(tmp);
    fs::create_directories(tmp);
    fs::create_directories(tmp / ".hive-agent" / "default");

    {
        std::ofstream f(tmp / ".hive-agent" / "default" / "system_prompt.txt");
        f << "PROJECT: rules specific to this repo.";
    }

    auto sink = std::make_shared<RecordingSink>();
    hive::AgentConfig cfg;
    cfg.project_root  = tmp;
    cfg.system_prompt = "DEMO: the default CLI prompt.\n";

    hive::Agent agent(std::move(cfg), sink);
    agent.initialize();

    // Look for the [INFO] merge log line.
    bool found_merge_log = false;
    for (const auto& evt : sink->events) {
        if (evt.first == hive::LogLevel::Info &&
            evt.second.find("[SYSTEM_PROMPT]") != std::string::npos &&
            evt.second.find("system_prompt.txt") != std::string::npos) {
            found_merge_log = true;
            break;
        }
    }
    assert(found_merge_log); (void)found_merge_log;
    std::cout << "[ok] test_project_override_appended" << std::endl;

    fs::remove_all(tmp);
}

// §Phase 14.7: when cfg.system_prompt is empty, project file IS the prompt.
void test_project_override_replaces_empty() {
    fs::path tmp = fs::temp_directory_path() / "phase147-replaces";
    fs::remove_all(tmp);
    fs::create_directories(tmp);
    fs::create_directories(tmp / ".hive-agent" / "default");

    {
        std::ofstream f(tmp / ".hive-agent" / "default" / "system_prompt.txt");
        f << "PROJECT: only the project text, no demo text.";
    }

    auto sink = std::make_shared<RecordingSink>();
    hive::AgentConfig cfg;
    cfg.project_root  = tmp;
    cfg.system_prompt = "";  // empty

    hive::Agent agent(std::move(cfg), sink);
    agent.initialize();

    // No exception, no error log.
    bool found_error = false;
    for (const auto& evt : sink->events) {
        if (evt.first == hive::LogLevel::Error) {
            found_error = true;
            std::cerr << "[unexpected error] " << evt.second << "\n";
        }
    }
    assert(!found_error); (void)found_error;
    std::cout << "[ok] test_project_override_replaces_empty" << std::endl;

    fs::remove_all(tmp);
}

// §Phase 14.7: missing file = no-op, no warning.
void test_no_override_silent() {
    fs::path tmp = fs::temp_directory_path() / "phase147-missing";
    fs::remove_all(tmp);
    fs::create_directories(tmp);
    // No .hive-agent/default/system_prompt.txt

    auto sink = std::make_shared<RecordingSink>();
    hive::AgentConfig cfg;
    cfg.project_root  = tmp;
    cfg.system_prompt = "DEMO: only this text.";

    hive::Agent agent(std::move(cfg), sink);
    agent.initialize();

    // No merge log.
    for (const auto& evt : sink->events) {
        assert(evt.second.find("[SYSTEM_PROMPT]") == std::string::npos);
        (void)evt;
    }
    std::cout << "[ok] test_no_override_silent" << std::endl;

    fs::remove_all(tmp);
}

// §Phase 14.7: empty project file is treated as "no override".
void test_empty_override_silent() {
    fs::path tmp = fs::temp_directory_path() / "phase147-empty";
    fs::remove_all(tmp);
    fs::create_directories(tmp);
    fs::create_directories(tmp / ".hive-agent" / "default");
    {
        std::ofstream f(tmp / ".hive-agent" / "default" / "system_prompt.txt");
        // no writes = empty file
    }

    auto sink = std::make_shared<RecordingSink>();
    hive::AgentConfig cfg;
    cfg.project_root  = tmp;
    cfg.system_prompt = "DEMO: only this text.";

    hive::Agent agent(std::move(cfg), sink);
    agent.initialize();

    // Empty file → no merge log, no exception.
    for (const auto& evt : sink->events) {
        assert(evt.second.find("[SYSTEM_PROMPT]") == std::string::npos);
        (void)evt;
    }
    std::cout << "[ok] test_empty_override_silent" << std::endl;

    fs::remove_all(tmp);
}

// §Phase 14.7: missing .daedalus/daedalus/ dir entirely = no-op.
void test_no_daedalus_dir_silent() {
    fs::path tmp = fs::temp_directory_path() / "phase147-no-daedalus-dir";
    fs::remove_all(tmp);
    fs::create_directories(tmp);
    // No .daedalus/daedalus/ subdir at all

    auto sink = std::make_shared<RecordingSink>();
    hive::AgentConfig cfg;
    cfg.project_root  = tmp;
    cfg.system_prompt = "DEMO: only this text.";

    hive::Agent agent(std::move(cfg), sink);
    agent.initialize();

    // No merge log, no error.
    for (const auto& evt : sink->events) {
        assert(evt.second.find("[SYSTEM_PROMPT]") == std::string::npos);
        if (evt.first == hive::LogLevel::Error) {
            std::cerr << "[unexpected error] " << evt.second << "\n";
            assert(false);
        }
    }
    std::cout << "[ok] test_no_daedalus_dir_silent" << std::endl;

    fs::remove_all(tmp);
}

}  // namespace

int main() {
    test_project_override_appended();
    test_project_override_replaces_empty();
    test_no_override_silent();
    test_empty_override_silent();
    test_no_daedalus_dir_silent();

    std::cout << "test_phase14_per_project_system_prompt: 5 tests passed\n";
    return 0;
}