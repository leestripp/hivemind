#include "SoundFx.h"

#include "AudioCommon.h"
#include "AudioPlayback.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace hv {

namespace {

// ~0.21 of full scale — clearly audible over speech, not harsh.
constexpr int kAmp = 7000;
constexpr int kGapSamples = kSampleRate * 25 / 1000;  // 25 ms between blips

// One sine blip, `ms` long, with 5 ms linear fades on both edges — a
// hard cut at arbitrary amplitude is the classic click.
Pcm16 blip(float hz, int ms) {
    const int n = kSampleRate * ms / 1000;
    const int fade = std::min(kSampleRate / 200, n / 4);
    Pcm16 out(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i) {
        float env = 1.0f;
        if (i < fade) {
            env = static_cast<float>(i + 1) / (fade + 1);
        } else if (i > n - fade) {
            env = static_cast<float>(n - i) / (fade + 1);
        }
        out[static_cast<std::size_t>(i)] = static_cast<int16_t>(
            kAmp * env *
            std::sin(2.0f * std::numbers::pi_v<float> * hz *
                     static_cast<float>(i) / kSampleRate));
    }
    return out;
}

// Two blips separated by a short silence gap — reads as "beep-beep".
Pcm16 twoTone(float hz1, int ms1, float hz2, int ms2) {
    Pcm16 out = blip(hz1, ms1);
    out.insert(out.end(), kGapSamples, 0);
    const Pcm16 second = blip(hz2, ms2);
    out.insert(out.end(), second.begin(), second.end());
    return out;
}

}  // namespace

SoundFx::SoundFx() : playback_(std::make_unique<AudioPlayback>()) {
    playback_->start();
}

SoundFx::~SoundFx() = default;  // ~AudioPlayback() halts the worker

void SoundFx::recordingStarted() const {
    // A5 -> E6: rising pair, "mic open".
    playback_->submit(0, twoTone(880.0f, 60, 1318.5f, 90), kSampleRate);
}

void SoundFx::recordingStopped() const {
    // B5 -> E5: falling pair, "mic closed" (SEND or CANCEL).
    playback_->submit(0, twoTone(987.8f, 70, 659.3f, 100), kSampleRate);
}

}  // namespace hv
