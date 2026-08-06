// test_daedalus_smoke.cpp
// -------------------------------------------------------------------------
// Minimal compile/link/runtime smoke test for the daedalus embedding.
// Constructs a daedalus::Agent with a do-nothing sink and a throwaway
// configuration, then destroys it. Does NOT call initialize() or run
// anything, so no network/filesystem access is required.

#include <cassert>
#include <iostream>
#include <memory>
#include <string>

#include <daedalus.h>

namespace {

class NullSink final : public daedalus::AgentSink {};

} // namespace

int main() {
    daedalus::AgentConfig cfg;
    cfg.project_root   = "/tmp/hivemind-daedalus-smoke";
    cfg.agent_name     = "smoke";
    cfg.model_name     = "nonexistent-model-for-smoke-test";
    cfg.api_key        = "dummy-key";
    cfg.api_host       = "https://example.invalid";
    cfg.chat_endpoint  = "/v1/chat/completions";
    cfg.max_context_size = 1024;

    auto sink = std::make_shared<NullSink>();

    {
        daedalus::Agent agent{cfg, sink};
        // Construct and destruct without initializing; no network I/O.
        (void)agent;
    }

    std::cout << "daedalus smoke test passed\n";
    return 0;
}
