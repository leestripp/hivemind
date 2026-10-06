#include "TtsEngine.h"

#include "AudioPlayback.h"
#include "ChimpFlowClient.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace hv {

TtsEngine::TtsEngine(AudioPlayback& playback, std::string chimpflowBase,
                     std::string chimpflowKey)
    : playback_(playback),
      chimp_(std::make_unique<ChimpFlowClient>(std::move(chimpflowBase),
                                               std::move(chimpflowKey))) {
    playback_.setOnTagDone([this](std::uint64_t tag) { onTagDone(tag); });
    {
        std::lock_guard<std::mutex> lk(m_);
        running_ = true;
    }
    worker_ = std::thread([this] { workerLoop(); });
}

TtsEngine::~TtsEngine() {
    {
        std::lock_guard<std::mutex> lk(m_);
        running_ = false;
        cv_.notify_all();
    }
    if (worker_.joinable()) worker_.join();
}

void TtsEngine::applyVoice(const std::string& voice) {
    std::lock_guard<std::mutex> lk(m_);
    voice_ = voice;
}

std::string TtsEngine::voice() const {
    std::lock_guard<std::mutex> lk(m_);
    return voice_;
}

void TtsEngine::speakQueued(const std::string& text) {
    if (text.empty()) return;
    {
        std::lock_guard<std::mutex> lk(m_);
        if (queue_.size() >= 40) return;  // never-ready cap, as Android
        queue_.push_back({epoch_, nextTag_++, text, voice_});
    }
    outstanding_++;
    cv_.notify_one();
}

void TtsEngine::stop() {
    {
        std::lock_guard<std::mutex> lk(m_);
        epoch_++;  // in-flight tags from before are now stale
        queue_.clear();
    }
    outstanding_ = 0;
    speaking_ = false;
    playback_.clear();
}

void TtsEngine::preview(const std::string& text, const std::string& voice) {
    stop();  // QUEUE_FLUSH: interrupt anything playing
    DecodedWav d = synthesize(text, voice);
    const std::uint64_t tag = [&] {
        std::lock_guard<std::mutex> lk(m_);
        return nextTag_++;
    }();
    if (!d.pcm.empty()) playback_.submit(tag, std::move(d.pcm), d.rate);
}

float TtsEngine::level(float nowSec) const {
    if (!speaking_) return 0.0f;
    // Wide envelope: the sphere's brightness floor is deliberately low,
    // so this swing IS the visual voice — let it breathe across most
    // of the 0..1 range instead of hovering mid-band.
    const float base = 0.42f + 0.36f * std::sin(nowSec * 7.3f) *
                                   std::sin(nowSec * 2.1f + 1.3f);
    return std::clamp(base, 0.05f, 0.90f);
}

std::vector<std::string> TtsEngine::voices() { return chimp_->voices(); }

void TtsEngine::workerLoop() {
    for (;;) {
        Utterance u;
        {
            std::unique_lock<std::mutex> lk(m_);
            cv_.wait(lk, [&] { return !running_ || !queue_.empty(); });
            if (!running_) return;
            u = std::move(queue_.front());
            queue_.pop_front();
        }
        const std::uint64_t epochAtSpawn = u.epoch;
        DecodedWav d = synthesize(u.text, u.voice);
        // A stop() landed while the request was in flight: the
        // utterance is dead, its tag is stale — do not touch the queue
        // accounting.
        {
            std::lock_guard<std::mutex> lk(m_);
            if (epoch_ != epochAtSpawn) continue;
        }
        if (!d.pcm.empty()) {
            playback_.submit(u.tag, std::move(d.pcm), d.rate);
        } else {
            // Synthesis failed: no playback item will ever carry this
            // tag, so complete the queue slot HERE. Without this the
            // speakQueued() increment of outstanding_ leaks — the app
            // then waits for an onQueueEmpty that never comes and the
            // turn never returns to Idle (Stop chrome stuck on screen
            // long after the agent finished; polling has already
            // stopped because the agent went idle).
            onTagDone(u.tag);
        }
    }
}

void TtsEngine::onTagDone(std::uint64_t tag) {
    // Late callbacks from a stop()ed epoch are harmless: stop() already
    // zeroed outstanding_, so this decrement never fires below zero.
    (void)tag;
    if (outstanding_ > 0) --outstanding_;
    if (outstanding_ == 0) {
        speaking_ = false;
        if (onQueueEmpty) onQueueEmpty();
    }
}

DecodedWav TtsEngine::synthesize(const std::string& text,
                                  const std::string& voice) {
    try {
        DecodedWav d = chimp_->synthesize(text, voice);
        speaking_ = true;
        return d;
    } catch (const std::exception& e) {
        // Synthesis failed — silence, not noise; the error is already
        // logged by the client (with its HTTP status).
        fprintf(stderr, "hive-voice: tts utterance failed: %s\n", e.what());
        return {};
    }
}

}  // namespace hv
