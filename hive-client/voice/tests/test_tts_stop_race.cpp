#include "AudioPlayback.h"
#include "TtsEngine.h"
#include "WavCodec.h"

#include <OmniNet.h>
#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>

namespace {

// Delays one real ChimpFlow HTTP response so the test can call stop()
// while synthesis is in flight, then deliver valid WAV afterwards.
class DelayedSpeechServer {
public:
    explicit DelayedSpeechServer(bool succeed = true) : succeed_(succeed) {
        OmniNet::HTTP::Server::Config cfg;
        cfg.address = "127.0.0.1";
        cfg.port = 18994;
        cfg.logRequests = false;
        server_ = std::make_unique<OmniNet::HTTP::Server>(cfg);
        server_->post("/v1/audio/speech",
                      [this](const OmniNet::HTTP::Request&,
                             OmniNet::HTTP::Response& res) {
                          {
                              std::lock_guard lock{m_};
                              started_ = true;
                          }
                          cv_.notify_all();
                          {
                              std::unique_lock lock{m_};
                              if (!cv_.wait_for(lock, std::chrono::seconds{5},
                                                [this] { return release_; })) {
                                  res.setStatus(504);
                                  res.setBody("test timed out");
                                  return;
                              }
                          }
                          if (!succeed_) {
                              res.setStatus(500);
                              res.setBody("{\"error\":\"test failure\"}");
                          } else {
                              const std::vector<std::int16_t> samples(1600, 0);
                              res.setBody(hv::encodeWav16kMono(samples.data(),
                                                               samples.size()));
                          }
                          {
                              std::lock_guard lock{m_};
                              served_ = true;
                          }
                          cv_.notify_all();
                      });
        thread_ = std::jthread([this] { server_->start(); });
        std::this_thread::sleep_for(std::chrono::milliseconds{150});
    }

    ~DelayedSpeechServer() {
        releaseResponse();
        server_->stop();
    }

    bool waitUntilStarted() {
        std::unique_lock lock{m_};
        return cv_.wait_for(lock, std::chrono::seconds{5},
                            [this] { return started_; });
    }

    void releaseResponse() {
        {
            std::lock_guard lock{m_};
            release_ = true;
        }
        cv_.notify_all();
    }

    bool waitUntilServed() {
        std::unique_lock lock{m_};
        return cv_.wait_for(lock, std::chrono::seconds{5},
                            [this] { return served_; });
    }

private:
    std::unique_ptr<OmniNet::HTTP::Server> server_;
    std::jthread thread_;
    std::mutex m_;
    std::condition_variable cv_;
    bool started_ = false;
    bool release_ = false;
    bool served_ = false;
    bool succeed_ = true;
};

} // namespace

TEST_CASE("TTS stop survives a delayed synthesis response", "[voice-tts]") {
    DelayedSpeechServer server;
    hv::AudioPlayback playback;
    playback.start();
    hv::TtsEngine tts{playback, "http://127.0.0.1:18994", "test-key"};
    tts.speakQueued("hello");
    REQUIRE(server.waitUntilStarted());

    tts.stop();
    REQUIRE(tts.outstanding() == 0);
    REQUIRE_FALSE(tts.speaking());

    server.releaseResponse();
    REQUIRE(server.waitUntilServed());
    // The stale worker must finish decoding and discover its cancelled epoch
    // without reasserting speaking_. A real HTTP round trip has completed;
    // this short grace period covers the worker waking and consuming it.
    std::this_thread::sleep_for(std::chrono::milliseconds{200});
    REQUIRE(tts.outstanding() == 0);
    REQUIRE_FALSE(tts.speaking());
    playback.stop();
}

TEST_CASE("TTS failure completes its outstanding tag", "[voice-tts]") {
    DelayedSpeechServer server{false};
    hv::AudioPlayback playback;
    playback.start();
    hv::TtsEngine tts{playback, "http://127.0.0.1:18994", "test-key"};
    tts.speakQueued("hello");
    REQUIRE(server.waitUntilStarted());
    server.releaseResponse();
    REQUIRE(server.waitUntilServed());

    bool drained = false;
    for (int i = 0; i < 100; ++i) {
        if (tts.outstanding() == 0) {
            drained = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{20});
    }
    REQUIRE(drained);
    REQUIRE_FALSE(tts.speaking());
    playback.stop();
}
