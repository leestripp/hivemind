#pragma once

#include "AudioCommon.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace hv {

class AudioCapture;
class ChimpFlowClient;

// Mic capture + transcription via the ChimpFlow gateway (whisper.cpp
// over localhost HTTP). No local models, no engine init: start()
// captures 16 kHz mono chunks, and stop() uploads the recording to
// POST /v1/audio/transcriptions and keeps the returned text. VAD-gated,
// no endpointer: recording stops only on stop() (SEND) or discard()
// (CANCEL). There is deliberately no live partial transcription —
// partials would re-upload the whole buffer every few seconds and
// whisper.cpp is not fast enough for that to feel live.
class SttEngine {
 public:
    SttEngine(AudioCapture& capture, std::string chimpflowBase,
              std::string chimpflowKey,
              std::function<void(float)> onLevel,
              std::function<void(const std::string&)> onStopped);
    ~SttEngine();
    SttEngine(const SttEngine&) = delete;
    SttEngine& operator=(const SttEngine&) = delete;

    bool engineReady() const { return ready_; }
    bool listening() const { return listening_; }
    std::string transcript() const;

    // Background warm-up at app launch: probe the gateway until it
    // answers so the first TALK after a reboot is instant and the app
    // self-heals when the voice service starts after the app does.
    // Never touches the mic and never surfaces an error.
    void prime();

    // TALK: never a silent no-op — arms capture when not ready.
    void start();
    // SEND: stop capturing, upload for the final transcription, keep
    // the transcript. Async: onFinalized fires on a worker thread once
    // transcript() is final.
    void stop(std::function<void()> onFinalized = {});
    // CANCEL: abort and discard everything.
    void discard();

 private:
    void startCapture();
    void teardownCapture();
    // /healthz probe with 5 s backoff (12 attempts ≈ 1 min). When
    // captureOnReady is set this came from start(): bail if the session
    // was cancelled, arm capture on success, and surface the failure via
    // on_stopped_ after the last attempt. prime() passes false and only
    // flips ready_. Every attempt is logged — a silent voice outage must
    // never happen again (2026-10-08 post-reboot incident).
    void readinessLoop(int session, bool captureOnReady);

    AudioCapture& capture_;
    std::unique_ptr<ChimpFlowClient> chimp_;
    std::function<void(float)> on_level_;
    std::function<void(const std::string&)> on_stopped_;

    mutable std::mutex m_;
    std::vector<int16_t> pcm_;  // growing buffer, capped at MAX
    std::size_t pcm_len_ = 0;   // under m_
    std::string transcript_;    // under m_

    std::atomic<bool> ready_{false};
    std::atomic<bool> want_listen_{false};
    std::atomic<bool> listening_{false};
    std::atomic<int> session_{0};
};

}  // namespace hv
