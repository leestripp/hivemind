#pragma once

#include "AudioCommon.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

struct whisper_context;

namespace hv {

class AudioCapture;

// On-device STT via whisper.cpp — a 1:1 port of the Android SttEngine
// (ggml-tiny.en partials + ggml-base.en final, sliding-window partial
// re-transcription every 2 s, VAD-gated, no endpointer: recording stops
// only on stop() (SEND) or discard() (CANCEL)).
//
// Models live in ~/.local/share/hive-client-voice/models/ (see
// modelDir()); a missing model is a loud error, never a silent no-op.
// Mic chunks arrive via AudioCapture; all whisper work runs on a worker
// thread (ASR never touches the UI thread).
class SttEngine {
 public:
    SttEngine(AudioCapture& capture,
              std::function<void(float)> onLevel,
              std::function<void(const std::string&)> onTranscript,
              std::function<void(const std::string&)> onStopped);
    ~SttEngine();
    SttEngine(const SttEngine&) = delete;
    SttEngine& operator=(const SttEngine&) = delete;

    static std::string modelDir();

    bool engineReady() const { return ready_; }
    bool listening() const { return listening_; }
    std::string transcript() const;

    // TALK: never a silent no-op — arms the engine when not ready.
    void start();
    // SEND: stop capturing, run the full final decode, keep the
    // transcript. Async: onFinalized fires on a worker thread once
    // transcript() is final.
    void stop(std::function<void()> onFinalized = {});
    // CANCEL: abort and discard everything.
    void discard();

 private:
    void initEngine();
    void startCapture();
    void teardownCapture();
    void decodePartial();
    void partialLoop();
    void workerInit();

    AudioCapture& capture_;
    std::function<void(float)> on_level_;
    std::function<void(const std::string&)> on_transcript_;
    std::function<void(const std::string&)> on_stopped_;

    mutable std::mutex m_;
    std::vector<int16_t> pcm_;  // growing buffer, capped at MAX
    std::size_t pcm_len_ = 0;   // under m_
    std::string transcript_;    // under m_

    std::atomic<bool> ready_{false};
    std::atomic<bool> want_listen_{false};
    std::atomic<bool> listening_{false};
    std::atomic<int> session_{0};
    std::atomic<bool> decode_in_flight_{false};
    std::atomic<bool> running_{false};

    std::thread worker_;   // engine init + capture + final decode
    std::thread ticker_;   // partial decode every PARTIAL_MS
    whisper_context* ctx_partial_ = nullptr;
    whisper_context* ctx_final_ = nullptr;
};

}  // namespace hv
