#include "ApiClient.h"

#include <OmniNet.h>
#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <string>
#include <thread>

namespace {
class ActivityServer {
public:
    ActivityServer() {
        OmniNet::HTTP::Server::Config cfg;
        cfg.address = "127.0.0.1";
        cfg.port = 18993;
        cfg.logRequests = false;
        server_ = std::make_unique<OmniNet::HTTP::Server>(cfg);
        server_->get("/api/agents/activity",
                     [](const OmniNet::HTTP::Request&,
                        OmniNet::HTTP::Response& res) {
                         res.setBody(R"({"agents":{
                             "bob":{"busy":true,"partial_len":42},
                             "forge":{"busy":true,"partial_len":7}
                         }})");
                     });
        thread_ = std::jthread([this] { server_->start(); });
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }

    ~ActivityServer() { server_->stop(); }

private:
    std::unique_ptr<OmniNet::HTTP::Server> server_;
    std::jthread thread_;
};
} // namespace

TEST_CASE("agent activity reads the hub's agents envelope", "[voice-api]") {
    ActivityServer server;
    hv::ApiClient api{"http://127.0.0.1:18993", "test-key"};

    // Regression: agentBusy searched the response root for the agent id,
    // making every agent permanently idle. The voice UI then ended a live
    // turn, restarted idle narration, and repeated its partial forever.
    REQUIRE(api.agentBusy("bob"));
    REQUIRE(api.agentBusy("forge"));
    REQUIRE_FALSE(api.agentBusy("missing"));
}
