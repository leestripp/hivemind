#include "ChimpFlowClient.h"
#include "WavCodec.h"

#include <OmniNet.h>
#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

class CatalogServer {
public:
    CatalogServer() {
        OmniNet::HTTP::Server::Config cfg;
        cfg.address = "127.0.0.1";
        cfg.port = 18995;
        cfg.logRequests = false;
        server_ = std::make_unique<OmniNet::HTTP::Server>(cfg);
        server_->get("/healthz", [](const OmniNet::HTTP::Request&,
                                    OmniNet::HTTP::Response& res) {
            res.setContentType("application/json");
            res.setBody(R"({"status":"ok","service":"chimpflow"})");
        });
        server_->get("/v1/models", [](const OmniNet::HTTP::Request& req,
                                      OmniNet::HTTP::Response& res) {
            if (req.headerOr("Authorization", "") != "Bearer test-key") {
                res.setStatus(OmniNet::HTTP::StatusCode::UNAUTHORIZED);
                res.setContentType("application/json");
                res.setBody(R"({"error":"invalid api key"})");
                return;
            }
            res.setContentType("application/json");
            res.setBody(R"({"object":"list","data":[
                {"id":"kokoro-82m","task":"tts"},
                {"id":"chatterbox","task":"clon"}
            ]})");
        });
        server_->get("/v1/voices", [this](const OmniNet::HTTP::Request& req,
                                          OmniNet::HTTP::Response& res) {
            const auto model = req.queryParameter("model").value_or("");
            {
                std::lock_guard lock{m_};
                last_voice_model_ = model;
            }
            res.setContentType("application/json");
            if (model == "kokoro-82m")
                res.setBody(R"({"data":[{"id":"af_heart"},{"id":"am_adam"}]})");
            else if (model == "chatterbox")
                res.setBody(R"({"data":[{"id":"reference-1"}]})");
            else if (model == "remote/model +?")
                res.setBody(R"({"data":[{"id":"encoded-voice"}]})");
            else
                res.setBody(R"({"data":[]})");
        });
        server_->post("/v1/audio/speech",
                      [this](const OmniNet::HTTP::Request& req,
                             OmniNet::HTTP::Response& res) {
                          const auto body = nlohmann::json::parse(req.bodyAsString());
                          {
                              std::lock_guard lock{m_};
                              last_speech_request_ = body;
                          }
                          if (body.value("model", "") == "return-json") {
                              res.setContentType("application/json");
                              res.setBody(R"({"unexpected":"not audio"})");
                              return;
                          }
                          const std::vector<std::int16_t> samples(1600, 0);
                          res.setContentType("audio/wav");
                          res.setHeader("X-ChimpFlow-Model",
                                        body.value("model", ""));
                          res.setBody(hv::encodeWav16kMono(samples.data(),
                                                           samples.size()));
                      });
        thread_ = std::jthread([this] { server_->start(); });
        std::this_thread::sleep_for(std::chrono::milliseconds{150});
    }

    ~CatalogServer() { server_->stop(); }

    std::string lastVoiceModel() {
        std::lock_guard lock{m_};
        return last_voice_model_;
    }

    nlohmann::json lastSpeechRequest() {
        std::lock_guard lock{m_};
        return last_speech_request_;
    }

private:
    std::unique_ptr<OmniNet::HTTP::Server> server_;
    std::jthread thread_;
    std::mutex m_;
    std::string last_voice_model_;
    nlohmann::json last_speech_request_;
};

}  // namespace

TEST_CASE("ChimpFlow discovers models and model-scoped voices",
          "[chimpflow][api]") {
    CatalogServer server;
    hv::ChimpFlowClient client{"http://127.0.0.1:18995", "test-key"};

    REQUIRE(client.healthy());
    // Chatterbox is a speech choice despite its engine task being "clon";
    // discovery must not assume every usable model has task="tts".
    REQUIRE(client.models() ==
            std::vector<std::string>{"kokoro-82m", "chatterbox"});
    REQUIRE(client.voices("kokoro-82m") ==
            std::vector<std::string>{"af_heart", "am_adam"});
    REQUIRE(client.voices("chatterbox") ==
            std::vector<std::string>{"reference-1"});

    REQUIRE(client.voices("remote/model +?") ==
            std::vector<std::string>{"encoded-voice"});
    REQUIRE(server.lastVoiceModel() == "remote/model +?");
}

TEST_CASE("ChimpFlow sends the selected model and voice together",
          "[chimpflow][api]") {
    CatalogServer server;
    hv::ChimpFlowClient client{"http://127.0.0.1:18995", "test-key"};

    const auto decoded = client.synthesize("Hello", "kokoro-82m", "af_heart");
    REQUIRE(decoded.rate == 16000);
    REQUIRE(decoded.pcm.size() == 1600);
    const auto request = server.lastSpeechRequest();
    REQUIRE(request.value("model", "") == "kokoro-82m");
    REQUIRE(request.value("voice", "") == "af_heart");
    REQUIRE(request.value("input", "") == "Hello");
    REQUIRE(request.value("response_format", "") == "wav");

    REQUIRE_THROWS_AS(client.synthesize("Hello", "return-json", ""),
                      hv::ChimpFlowError);
}

TEST_CASE("ChimpFlow discovery surfaces authentication failures",
          "[chimpflow][api]") {
    CatalogServer server;
    hv::ChimpFlowClient client{"http://127.0.0.1:18995", "bad-key"};
    REQUIRE_THROWS_AS(client.models(), hv::ChimpFlowError);
    REQUIRE_THROWS_AS(client.voices(""), hv::ChimpFlowError);
}
