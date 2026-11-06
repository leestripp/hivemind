#pragma once

#include "AudioCommon.h"
#include "TtsTagLedger.h"
#include "WavCodec.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace hv {

class AudioPlayback;
class ChimpFlowClient;

// Queued-utterance TTS for streamed replies, backed by the ChimpFlow
// gateway (audio.cpp pocket-tts over localhost HTTP). Same
// sentence-queue contract as before: speakQueued() while the agent
// streams, onQueueEmpty fires once when everything queued has played.
//
// Each utterance is one POST /v1/audio/speech; the returned WAV is
// decoded to 16 kHz mono s16le and submitted to AudioPlayback under a
// monotonically increasing tag. Voices are ChimpFlow model ids
// ("" = engine default); per-agent assignments come from Prefs.
class TtsEngine {
 public:
    TtsEngine(AudioPlayback& playback, std::string chimpflowBase,
              std::string chimpflowKey);
    ~TtsEngine();
    TtsEngine(const TtsEngine&) = delete;
    TtsEngine& operator=(const TtsEngine&) = delete;

    // Applied per queued utterance (the request's `model` field);
    // "" = engine default.
    void applyVoice(const std::string& voice);
    std::string voice() const;

    // Queue a chunk for speech (from any thread).
    void speakQueued(const std::string& text);

    // Interrupt: drop everything queued + playing; late completion
    // callbacks from before the stop are ignored via the epoch counter
    // (the Android epochBase trick).
    void stop();

    // One-off sample playback (voice picker); interrupts anything prior.
    void preview(const std::string& text, const std::string& voice);

    // Animated 0..1 envelope for the sphere while the agent talks.
    float level(float nowSec) const;

    int outstanding() const { return outstanding_; }
    bool speaking() const { return speaking_; }

    // TTS voices the gateway currently serves (GET /v1/models); empty
    // when the gateway is unreachable.
    std::vector<std::string> voices();

    std::function<void()> onQueueEmpty;  // fires on the playback thread

 private:
    struct Utterance {
        std::uint64_t epoch;
        std::uint64_t tag;
        std::string text;
        std::string voice;  // captured at queue time
    };

    void workerLoop();
    void onTagDone(std::uint64_t tag);
    // POST to ChimpFlow and decode the WAV; {} on failure (logged).
    DecodedWav synthesize(const std::string& text, const std::string& voice);

    AudioPlayback& playback_;
    std::unique_ptr<ChimpFlowClient> chimp_;
    mutable std::mutex m_;
    std::deque<Utterance> queue_;
    TtsTagLedger pending_;
    std::condition_variable cv_;
    bool running_ = false;
    std::thread worker_;

    std::string voice_;  // under m_
    std::uint64_t epoch_ = 0;
    std::uint64_t nextTag_ = 1;
    std::atomic<int> outstanding_{0};
    std::atomic<bool> speaking_{false};
};

}  // namespace hv
