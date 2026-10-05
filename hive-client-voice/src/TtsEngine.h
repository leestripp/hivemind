#pragma once

#include "AudioCommon.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace hv {

class AudioPlayback;

// Queued-utterance TTS for streamed replies, backed by a piper
// subprocess (the desktop stand-in for Android's TextToSpeech engine —
// same sentence-queue contract: speakQueued() while the agent streams,
// onQueueEmpty fires once when everything queued has played).
//
// Each utterance spawns `piper --model <voice> --output-raw`, is fed the
// sentence on stdin, and stdout (raw s16le PCM, EOF = utterance end) is
// submitted to AudioPlayback under a monotonically increasing tag.
// Voices are per-agent piper model (.onnx) paths from Prefs::voiceDir().
class TtsEngine {
 public:
    struct VoiceOption {
        std::string path;   // .onnx model path
        std::string label;  // stem for display
    };

    explicit TtsEngine(AudioPlayback& playback);
    ~TtsEngine();
    TtsEngine(const TtsEngine&) = delete;
    TtsEngine& operator=(const TtsEngine&) = delete;

    // Applied per spawned utterance (piper --model); "" = engine default.
    void applyVoice(const std::string& modelPath);
    std::string voice() const;

    // Queue a chunk for speech (from any thread).
    void speakQueued(const std::string& text);

    // Interrupt: drop everything queued + playing; late completion
    // callbacks from before the stop are ignored via the epoch counter
    // (the Android epochBase trick).
    void stop();

    // One-off sample playback (voice picker); interrupts anything prior.
    void preview(const std::string& text, const std::string& modelPath);

    // Animated 0..1 envelope for the sphere while the agent talks.
    float level(float nowSec) const;

    int outstanding() const { return outstanding_; }
    bool speaking() const { return speaking_; }

    // List .onnx voices in a directory (empty/missing dir → {}).
    static std::vector<VoiceOption> voiceOptions(const std::string& voiceDir);

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
    // Spawn piper, write text on stdin, read raw PCM to EOF.
    Pcm16 synthesize(const std::string& text, const std::string& voice);

    AudioPlayback& playback_;
    mutable std::mutex m_;
    std::deque<Utterance> queue_;
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
