#include "SttEngine.h"

#include "AudioCapture.h"
#include "ChimpFlowClient.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <thread>

namespace hv {

namespace {
constexpr int kMaxSeconds = 120;
constexpr size_t kMaxSamples = kSampleRate * kMaxSeconds;
}  // namespace

SttEngine::SttEngine(AudioCapture& capture, std::string chimpflowBase,
                     std::string chimpflowKey,
                     std::function<void(float)> onLevel,
                     std::function<void(const std::string&)> onStopped)
    : capture_(capture),
      chimp_(std::make_unique<ChimpFlowClient>(std::move(chimpflowBase),
                                               std::move(chimpflowKey))),
      on_level_(std::move(onLevel)),
      on_stopped_(std::move(onStopped)) {}

SttEngine::~SttEngine() {
    capture_.stop();
}

std::string SttEngine::transcript() const {
    std::lock_guard<std::mutex> lk(m_);
    return transcript_;
}

void SttEngine::prime() {
    // Launch-time warm-up: probe in the background so a gateway that
    // starts late (or a network stack still coming up after reboot) is
    // picked up without the user pressing TALK into a dead first attempt.
    if (ready_) return;
    readinessLoop(session_.load(), false);
}

void SttEngine::start() {
    want_listen_ = true;
    const int s = ++session_;
    // The only "readiness" left is the gateway answering /healthz --
    // check it off the UI thread so start() never blocks.
    if (!ready_) {
        readinessLoop(s, true);
        return;
    }
    startCapture();
}

void SttEngine::readinessLoop(const int s, const bool captureOnReady) {
    std::thread([this, s, captureOnReady] {
        constexpr int kMaxAttempts = 12;
        constexpr auto kBackoff = std::chrono::seconds(5);
        for (int attempt = 1; attempt <= kMaxAttempts; ++attempt) {
            // A start()-driven probe dies the moment the session is
            // cancelled; prime() only checks ready_ to avoid pile-up.
            if (captureOnReady && (!want_listen_ || s != session_)) return;
            if (!captureOnReady && ready_) return;
            if (chimp_->healthy()) {
                fprintf(stderr, "hive-voice: chimpflow healthy "
                                "(attempt %d)\n", attempt);
                ready_ = true;
                if (captureOnReady && want_listen_ && s == session_)
                    startCapture();
                return;
            }
            fprintf(stderr, "hive-voice: chimpflow not responding, "
                            "retry %d/%d in 5s\n", attempt, kMaxAttempts);
            std::this_thread::sleep_for(kBackoff);
        }
        if (captureOnReady && on_stopped_)
            on_stopped_("Voice service (ChimpFlow) is not "
                        "responding at " + chimp_->base() +
                        " -- is it running?");
    }).detach();
}
void SttEngine::startCapture() {
    if (!ready_ || listening_) return;
    {
        std::lock_guard<std::mutex> lk(m_);
        transcript_.clear();
        pcm_.assign(kSampleRate * 30, 0);
        pcm_len_ = 0;
    }
    const int s = session_.load();
    listening_ = capture_.start(
        [this, s](const int16_t* samples, std::size_t n) {
            if (s != session_) return;  // stale capture
            std::lock_guard<std::mutex> lk(m_);
            if (pcm_len_ + n > kMaxSamples) {
                const size_t keep = kMaxSamples / 2;
                std::copy_n(pcm_.data() + (pcm_len_ - keep), keep,
                            pcm_.data());
                pcm_len_ = keep;
            }
            if (pcm_len_ + n > pcm_.size())
                pcm_.resize(std::min(pcm_.size() * 2, kMaxSamples));
            std::copy_n(samples, n, pcm_.data() + pcm_len_);
            pcm_len_ += n;
        },
        [this, s](float level) {
            if (s == session_ && on_level_) on_level_(level);
        });
    if (on_recording_) on_recording_(listening_);
    if (!listening_ && on_stopped_)
        on_stopped_("Microphone unavailable — no audio capture backend");
}

void SttEngine::setRecordingListener(std::function<void(bool)> cb) {
    on_recording_ = std::move(cb);
}

void SttEngine::teardownCapture() {
    listening_ = false;
    if (on_recording_) on_recording_(false);
    capture_.stop();
}

void SttEngine::stop(std::function<void()> onFinalized) {
    want_listen_ = false;
    ++session_;
    std::thread([this, onFinalized = std::move(onFinalized)]() mutable {
        if (listening_) teardownCapture();
        std::vector<int16_t> snap;
        {
            std::lock_guard<std::mutex> lk(m_);
            snap.assign(pcm_.data(), pcm_.data() + pcm_len_);
        }
        if (!snap.empty()) {
            const auto t0 = std::chrono::steady_clock::now();
            try {
                std::string text = chimp_->transcribe(snap.data(),
                                                      snap.size());
                // Trim + set under the lock (last write wins, like the
                // Kotlin transcript var).
                const auto b = text.find_first_not_of(" \t\n\r");
                const auto e = text.find_last_not_of(" \t\n\r");
                if (b != std::string::npos) {
                    std::lock_guard<std::mutex> lk(m_);
                    transcript_ = text.substr(b, e - b + 1);
                }
            } catch (const std::exception& e) {
                // Transcription unavailable — transcript stays empty and
                // the UI says "Didn't catch that" (same shape as a
                // decode failure); the error itself is logged.
                fprintf(stderr, "hive-voice: stt transcription failed: %s\n",
                        e.what());
            }
            const auto dt = std::chrono::duration_cast<
                std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - t0).count();
            fprintf(stderr, "hive-voice: stt %lldms for %zus audio\n",
                    static_cast<long long>(dt), snap.size() / kSampleRate);
        }
        if (onFinalized) onFinalized();
    }).detach();
}

void SttEngine::discard() {
    want_listen_ = false;
    ++session_;
    {
        std::lock_guard<std::mutex> lk(m_);
        transcript_.clear();
        pcm_len_ = 0;
    }
    if (listening_) teardownCapture();
}

}  // namespace hv
