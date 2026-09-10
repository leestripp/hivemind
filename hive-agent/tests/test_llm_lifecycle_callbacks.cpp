// SPDX-License-Identifier: BSL-1.0
// tests/test_llm_lifecycle_callbacks.cpp
//
// Verifies the AgentSink LLM lifecycle callbacks that hosts use to drive
// a "thinking" spinner:
//   - on_llm_wait_started()
//   - on_llm_response_started()
//   - on_llm_request_finished()
//
// Hermetic portion (always run):
//   - With no model configured, no LLM lifecycle callbacks fire.
//   - A RecordingSink can override all three methods without crashing.
//
// Live-LLM portion (skipped unless HIVE_RUN_LIVE_LLM_TESTS=1):
//   - One streaming turn fires the hooks in order:
//       wait_started -> response_started -> [deltas] -> request_finished.
//   - Each hook fires at most once per request.

#include <algorithm>
#include <cassert>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "library/Agent.h"
#include "api/ProviderPlugin.h"
#include "api/ProviderRegistry.h"
#include "library/model_discovery.h"
#include "a2a/A2aClient.h"

namespace fs = std::filesystem;

namespace {

struct ScratchDir {
    fs::path path;
    explicit ScratchDir(const std::string& tag) {
        path = fs::temp_directory_path() /
               ("hive_agent_test_" + tag + "_" +
                std::to_string(reinterpret_cast<std::uintptr_t>(this)));
        fs::remove_all(path);
        fs::create_directories(path);
    }
    ~ScratchDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

class RecordingSink : public hive::AgentSink {
public:
    std::mutex mtx;
    int wait_started_count = 0;
    int response_started_count = 0;
    int request_finished_count = 0;
    int delta_count = 0;
    std::vector<std::string> events;  // chronological order

    void record(std::string name) {
        std::lock_guard<std::mutex> lock(mtx);
        events.push_back(std::move(name));
    }

    void on_llm_wait_started() noexcept override {
        ++wait_started_count;
        record("wait_started");
    }
    void on_llm_response_started() noexcept override {
        ++response_started_count;
        record("response_started");
    }
    void on_llm_request_finished() noexcept override {
        ++request_finished_count;
        record("request_finished");
    }
    void on_assistant_delta(std::string_view /*text*/) noexcept override {
        ++delta_count;
        record("delta");
    }
    void on_log(hive::LogLevel /*level*/, std::string_view /*msg*/) noexcept override {
        // Suppress log noise in test output.
    }
};

static bool live_llm_tests_enabled() {
    const char* env = std::getenv("HIVE_RUN_LIVE_LLM_TESTS");
    return env && (std::string(env) == "1" || std::string(env) == "true");
}

}  // namespace

int main() {
    auto sink = std::make_shared<RecordingSink>();
    ScratchDir dir("llm_lifecycle");

    // ---- Hermetic: no model configured ----
    {
        hive::AgentConfig cfg;
        cfg.project_root = dir.path;
        cfg.session_id   = "llm-lifecycle-no-model";
        cfg.agent_home = dir.path / "home";

        hive::Agent agent(std::move(cfg), sink);
        agent.initialize();
        agent.submit_user_message("hello");
        agent.run_one_turn();
        agent.shutdown();

        // No LLM client was created, so none of the lifecycle hooks should fire.
        assert(sink->wait_started_count == 0);
        assert(sink->response_started_count == 0);
        assert(sink->request_finished_count == 0);
    }

    if (!live_llm_tests_enabled()) {
        std::cerr << "[test] llm_lifecycle_callbacks hermetic PASSED; live-LLM portion skipped\n";
        return 77;
    }

    // ---- Live LLM: one streaming turn ----
    {
        hive::AgentConfig cfg;
        cfg.project_root = dir.path / "live";
        cfg.session_id   = "llm-lifecycle-live";
        cfg.model_name   = "minimax-m2.7";
        cfg.stream       = true;

        hive::Agent agent(std::move(cfg), sink);
        agent.initialize();

        // Reset counters from the hermetic portion.
        {
            std::lock_guard<std::mutex> lock(sink->mtx);
            sink->wait_started_count = 0;
            sink->response_started_count = 0;
            sink->request_finished_count = 0;
            sink->delta_count = 0;
            sink->events.clear();
        }

        agent.submit_user_message("Say exactly the word 'pong' and nothing else.");
        agent.run_one_turn();
        agent.shutdown();

        std::lock_guard<std::mutex> lock(sink->mtx);

        assert(sink->wait_started_count == 1);
        assert(sink->response_started_count == 1);
        assert(sink->request_finished_count == 1);

        // Order: wait must come before response, which must come before finish.
        // Deltas may be interleaved after response_started.
        const auto idx = [&](const std::string& name) -> std::size_t {
            auto it = std::find(sink->events.begin(), sink->events.end(), name);
            assert(it != sink->events.end());
            return static_cast<std::size_t>(it - sink->events.begin());
        };
        const std::size_t idx_wait = idx("wait_started");
        const std::size_t idx_response = idx("response_started");
        const std::size_t idx_finish = idx("request_finished");
        const bool order_ok = (idx_wait < idx_response) && (idx_response < idx_finish);
        assert(order_ok);
        (void)order_ok;

        // Streaming should have produced at least one delta.
        assert(sink->delta_count > 0);
    }

    std::cerr << "[test] llm_lifecycle_callbacks PASSED (hermetic + live-LLM)\n";
    return 0;
}
